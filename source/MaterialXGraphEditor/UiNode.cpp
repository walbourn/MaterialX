//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGraphEditor/UiNode.h>

namespace
{

const int INVALID_POS = -10000;

} // anonymous namespace

 void Pin::addConnection(Pin pin)
 {
     for (size_t i = 0; i < _connections.size(); i++)
     {
         if (_connections[i]._pinId == pin._pinId)
             return;
     }
     _connections.push_back(pin);
 }

UiNode::UiNode() :
    _level(-1),
    _showAllInputs(false),
    _id(0),
    _nodePos(INVALID_POS, INVALID_POS),
    _inputNodeNum(0)
{
}

UiNode::UiNode(const std::string name, int id) :
    _level(-1),
    _showAllInputs(false),
    _id(id),
    _nodePos(INVALID_POS, INVALID_POS),
    _name(name),
    _inputNodeNum(0)
{
}

UiNode::UiNode(int id) :
    _level(-1),
    _showAllInputs(false),
    _id(id),
    _nodePos(INVALID_POS, INVALID_POS),
    _inputNodeNum(0)
{
}

// return the uiNode connected with input name
UiNodePtr UiNode::getConnectedNode(std::string name)
{
    for (UiEdge edge : edges)
    {
        if (edge.getInputName() == name)
        {
            return edge.getDown();
        }
        else if (edge.getDown()->getName() == name)
        {
            return edge.getDown();
        }
    }
    for (UiEdge edge : edges)
    {

        if (edge.getInputName() == "")
        {

            return edge.getDown();
        }
    }
    return nullptr;
}

float UiNode::getAverageY()
{
    float small = 10000000.f;
    for (UiNodePtr node : _outputConnections)
    {
        ImVec2 pos = node->getPos();
        if (pos.y != INVALID_POS)
        {
            if (pos.y < small)
            {
                small = pos.x;
            }
        }
    }
    return small;
}

float UiNode::getMinX()
{
    float small = 10000000.f;
    for (UiNodePtr node : _outputConnections)
    {
        ImVec2 pos = node->getPos();
        if (pos.x != INVALID_POS)
        {
            if (pos.x < small)
            {
                small = pos.x;
            }
        }
    }
    return small;
}

int UiNode::getEdgeIndex(int id)
{
    int count = 0;
    for (UiEdge edge : edges)
    {
        if (edge.getUp()->getId() == id || edge.getDown()->getId() == id)
        {
            return count;
        }
        count++;
    }
    return -1;
}

void UiNode::removeOutputConnection(std::string name)
{
    for (size_t i = 0; i < _outputConnections.size(); i++)
    {
        if (_outputConnections[i]->getName() == name)
        {
            _outputConnections.erase(_outputConnections.begin() + i);
        }
    }
}

mx::ElementPtr UiNode::getMxElement()
{
    if (_currNode != nullptr)
    {
        return _currNode;
    }
    else if (_currInput != nullptr)
    {
        return _currInput;
    }
    else if (_currOutput != nullptr)
    {
        return _currOutput;
    }
    else
    {
        return nullptr;
    }
}
