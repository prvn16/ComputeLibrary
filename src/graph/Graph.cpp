/*
 * Copyright (c) 2017-2018 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/graph/Graph.h"

#include "arm_compute/graph/CL/CLMap.h"
#include "arm_compute/graph/CL/CLUnmap.h"
#include "arm_compute/graph/INode.h"
#include "arm_compute/graph/ITensorObject.h"
#include "arm_compute/graph/Tensor.h"
#include "arm_compute/runtime/CL/CLScheduler.h"
#include "arm_compute/runtime/CL/CLTensor.h"
#include "arm_compute/runtime/Tensor.h"
#include "support/ToolchainSupport.h"

#include <sys/stat.h>

using namespace arm_compute::graph;

namespace
{
bool file_exists(const std::string &filename)
{
    std::ifstream file(filename);
    return file.good();
}

} // namespace
struct Stage
{
    ITensorObject                          *_input;
    ITensorObject                          *_output;
    std::unique_ptr<arm_compute::IFunction> _function;
};

struct Graph::Private
{
public:
    /** Finalizes the current node's configuration
     *
     * @param _next_hint Device execution hint
     */
    void configure(GraphHints _next_hints);

    GraphContext                                _ctx{};
    std::vector<Stage>                          _pipeline{};
    std::vector<std::unique_ptr<ITensorObject>> _tensors{};
    std::vector<std::unique_ptr<INode>>         _nodes{};
    GraphHints                                  _current_hints{};
    GraphHints                                  _next_hints{};
    std::unique_ptr<ITensorObject>              _graph_input{ nullptr };
    std::unique_ptr<ITensorObject>              _graph_output{ nullptr };
    std::unique_ptr<INode>                      _current_node{ nullptr };
    ITensorObject                              *_current_output{ nullptr };
    bool                                        _info_enabled{ false };
    CLTuner                                     _tuner{};

private:
    ITensorObject *_current_input{ nullptr };
    GraphHints     _previous_hints{};
};

static const std::string tuner_data_filename = "acl_tuner.csv";
Graph::~Graph() //NOLINT
{
    if(_pimpl->_tuner.tune_new_kernels() && !_pimpl->_tuner.lws_table().empty())
    {
        _pimpl->_tuner.save_to_file(tuner_data_filename);
    }
}

Graph::Graph()
    : _pimpl{ new Private() }
{
    graph_init();
}

void Graph::graph_init(const bool use_cl_tuner)
{
    // Check if OpenCL is available and initialize the scheduler
    if(opencl_is_available())
    {
        if(_pimpl->_tuner.lws_table().empty() && file_exists(tuner_data_filename))
        {
            _pimpl->_tuner.load_from_file(tuner_data_filename);
        }
        _pimpl->_tuner.set_tune_new_kernels(use_cl_tuner);
        arm_compute::CLScheduler::get().default_init(&_pimpl->_tuner);
    }
}
void Graph::run()
{
    while(true)
    {
        if(_pimpl->_graph_input->has_accessor() && !_pimpl->_graph_input->call_accessor())
        {
            return;
        }

        for(auto &stage : _pimpl->_pipeline)
        {
            stage._function->run();
        }

        if((_pimpl->_graph_output->has_accessor() && !_pimpl->_graph_output->call_accessor())
           || (!_pimpl->_graph_output->has_accessor()))
        {
            return;
        }
    }
}

//Finalize current node's configuration
void Graph::Private::configure(GraphHints _next_hints)
{
    ARM_COMPUTE_ERROR_ON(_current_node == nullptr);
    ARM_COMPUTE_ERROR_ON(_graph_input == nullptr);

    // Is it the first node of the graph ?
    if(_current_input == nullptr)
    {
        _graph_input->set_target(_current_hints.target_hint());
        _current_input  = _graph_input.get();
        _previous_hints = _current_hints; // For the first node just assume the previous node was of the same type as this one
    }

    if(_current_node->supports_in_place())
    {
        _current_output = _current_input;
    }

    //Automatic output configuration ?
    if(_current_output == nullptr)
    {
        _tensors.push_back(arm_compute::support::cpp14::make_unique<Tensor>(TensorInfo()));
        _current_output = _tensors.back().get();
    }

    // If either the writer or reader node needs OpenCL then use OpenCL memory:
    if((_next_hints.target_hint() == TargetHint::OPENCL || _current_hints.target_hint() == TargetHint::OPENCL))
    {
        _current_output->set_target(TargetHint::OPENCL);
    }
    else
    {
        _current_output->set_target(TargetHint::NEON);
    }

    // Instantiate Node
    _ctx.hints()                                 = _current_hints;
    std::unique_ptr<arm_compute::IFunction> func = _current_node->instantiate_node(_ctx, _current_input, _current_output);

    // If the operation is done in-place, do not allocate or it will prevent following layers from performing the configuration
    if(!_current_node->supports_in_place())
    {
        // Allocate current input
        _current_input->allocate();
    }

    // Map input if needed
    if(_current_input->target() == TargetHint::OPENCL)
    {
        if(_previous_hints.target_hint() == TargetHint::NEON)
        {
            ARM_COMPUTE_ERROR_ON(_current_hints.target_hint() == TargetHint::NEON);
            _pipeline.push_back({ _current_input, _current_input, arm_compute::support::cpp14::make_unique<CLUnmap>(_current_input) });
        }
        if(_current_hints.target_hint() == TargetHint::NEON)
        {
            ARM_COMPUTE_ERROR_ON(_previous_hints.target_hint() == TargetHint::NEON);
            _pipeline.push_back({ _current_input, _current_input, arm_compute::support::cpp14::make_unique<CLMap>(_current_input, true) });
        }
    }

    _pipeline.push_back({ _current_input, _current_output, std::move(func) });

    _current_input  = _current_output;
    _current_output = nullptr;
    std::swap(_previous_hints, _current_hints);
    std::swap(_current_hints, _next_hints);
}

void Graph::add_node(std::unique_ptr<INode> node)
{
    ARM_COMPUTE_ERROR_ON_MSG(_pimpl->_graph_input == nullptr, "The graph's input must be set before the first node is added");
    ARM_COMPUTE_ERROR_ON_MSG(_pimpl->_graph_output != nullptr, "Nothing can be added after the output tensor");
    //Trigger the creation of the current Node:

    GraphHints _next_hints = _pimpl->_next_hints;
    _next_hints.set_target_hint(node->override_target_hint(_pimpl->_next_hints.target_hint()));
    ARM_COMPUTE_ERROR_ON(_next_hints.target_hint() == TargetHint::DONT_CARE);
    if(_pimpl->_current_node)
    {
        //Finalize the previous Node:
        _pimpl->configure(_pimpl->_next_hints);
    }
    else
    {
        // If that's the first node then use the same TargetHint before and after the node.
        _pimpl->_current_hints = _next_hints;
    }
    if(_pimpl->_current_node)
    {
        _pimpl->_nodes.push_back(std::move(_pimpl->_current_node));
    }
    _pimpl->_current_node = std::move(node);
}

//Add a tensor with an Accessor (i.e either the input or output of the graph)
void Graph::add_tensor_object(std::unique_ptr<ITensorObject> tensor)
{
    // If it's the first Tensor added then it will be the input of the Graph.
    if(_pimpl->_graph_input == nullptr)
    {
        ARM_COMPUTE_ERROR_ON(_pimpl->_graph_output != nullptr);
        ARM_COMPUTE_ERROR_ON(_pimpl->_current_node != nullptr);
        _pimpl->_graph_input = std::move(tensor);
    }
    else
    {
        // Else it will be the output of the Graph
        ARM_COMPUTE_ERROR_ON(_pimpl->_graph_output != nullptr);
        ARM_COMPUTE_ERROR_ON(_pimpl->_current_node == nullptr);
        _pimpl->_graph_output   = std::move(tensor);
        _pimpl->_current_output = _pimpl->_graph_output.get();

        // Finalize the graph by configuring the last Node of the graph:
        _pimpl->configure(_pimpl->_current_hints); // Ignore _next_hint as this is the last node, and just use the same hint as before this node.
        _pimpl->_graph_output->allocate();
    }
}

bool Graph::opencl_is_available()
{
    return arm_compute::opencl_is_available();
}

arm_compute::GPUTarget Graph::gpu_target()
{
    // Check if OpenCL is available before returning the GPU target
    if(opencl_is_available())
    {
        return arm_compute::CLScheduler::get().target();
    }
    else
    {
        return GPUTarget::MIDGARD;
    }
}

void Graph::set_temp(TensorInfo &&tmp)
{
    ARM_COMPUTE_ERROR_ON(_pimpl->_graph_input == nullptr);
    ARM_COMPUTE_ERROR_ON(_pimpl->_graph_output != nullptr);
    ARM_COMPUTE_ERROR_ON_MSG(_pimpl->_current_output != nullptr, "TensorInfo for temporary tensor already set");

    _pimpl->_tensors.push_back(arm_compute::support::cpp14::make_unique<Tensor>(std::move(tmp)));
    _pimpl->_current_output = _pimpl->_tensors.back().get();
}

GraphHints &Graph::hints()
{
    return _pimpl->_next_hints;
}

Graph &arm_compute::graph::operator<<(Graph &graph, TensorInfo &&info)
{
    graph.set_temp(std::move(info));
    return graph;
}

Graph &arm_compute::graph::operator<<(Graph &graph, Tensor &&tensor)
{
    graph.add_tensor_object(arm_compute::support::cpp14::make_unique<Tensor>(std::move(tensor)));
    return graph;
}

Graph &arm_compute::graph::operator<<(Graph &graph, SubTensor &&sub_tensor)
{
    graph.add_tensor_object(arm_compute::support::cpp14::make_unique<SubTensor>(std::move(sub_tensor)));
    return graph;
}

Graph &arm_compute::graph::operator<<(Graph &graph, TargetHint target_hint)
{
    graph.hints().set_target_hint(target_hint);
    return graph;
}

Graph &arm_compute::graph::operator<<(Graph &graph, ConvolutionMethodHint conv_method_hint)
{
    graph.hints().set_convolution_method_hint(conv_method_hint);
    return graph;
}
