
#pragma once

#include "ModelDefinition.hpp"

#include <memory>

namespace ModelOpcUa {

	//Instance of a node, contains the structur informations
	class Node : public NodeDefinition {
	public:
		using NodeDefinition::NodeDefinition;

		Node(const NodeDefinition &, std::list<std::shared_ptr<const Node>> childNodes);

		~Node() override = 0;

		/// All child elements
		const std::list<std::shared_ptr<const Node>> ChildNodes;
		bool ofBaseDataVariableType = false;
	protected:
	};

	// ModellingRule is one of {Optional, Mandatory}
	class SimpleNode : public Node {
	public:
		~SimpleNode() override = default;

		// Node available
		SimpleNode(
				NodeId_t nodeId,
				NodeId_t typeNodeId,
				const NodeDefinition& nodeDefinition,
				const std::list<std::shared_ptr<const Node>> &childNodes
		);

		// Node not available
		explicit SimpleNode(const NodeDefinition& nodeDefinition);

		bool isAvaliable() const;

		NodeId_t NodeId;

		// The instanciated type of the Node (might be a subtype of the defined one)
		NodeId_t TypeNodeId;
	};

	struct PlaceholderElement {
		std::shared_ptr<const SimpleNode> pNode;
		QualifiedName_t BrowseName;
	};

	// ModellingRule is one of {OptionalPlaceholder, MandatoryPlaceholder}
	class PlaceholderNode : public Node {
	public:
		using Node::Node;

		void addInstance(const PlaceholderElement& instance);

		std::list<PlaceholderElement> getInstances() const;

	protected:
		/// The found instances according to the predefined Type
		std::list<PlaceholderElement> Instances;
	};

}
