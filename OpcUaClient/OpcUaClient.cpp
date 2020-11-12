#include "OpcUaClient.hpp"

#include "SetupSecurity.hpp"


#include "uaplatformlayer.h"

#include <Exceptions/ClientNotConnected.hpp>
#include "Exceptions/OpcUaNonGoodStatusCodeException.hpp"

#include "Converter/ModelNodeIdToUaNodeId.hpp"
#include "Converter/UaNodeIdToModelNodeId.hpp"
#include "Converter/ModelQualifiedNameToUaQualifiedName.hpp"
#include "Converter/UaQualifiedNameToModelQualifiedName.hpp"
#include "Converter/UaNodeClassToModelNodeClass.hpp"
#include "Converter/UaDataValueToJsonValue.hpp"


namespace Umati {

	namespace OpcUa {

		int OpcUaClient::PlatformLayerInitialized = 0;

		OpcUaClient::OpcUaClient(std::string serverURI, std::string Username, std::string Password,
								 std::uint8_t security, std::vector<std::string> expectedObjectTypeNamespaces,
								 std::shared_ptr<Umati::OpcUa::OpcUaInterface> opcUaWrapper)
				: m_serverUri(std::move(serverURI)), m_username(std::move(Username)), m_password(std::move(Password)),
				  m_expectedObjectTypeNamespaces(std::move(expectedObjectTypeNamespaces)),
				  m_security(static_cast<OpcUa_MessageSecurityMode>(security)),
				  m_subscr(m_uriToIndexCache, m_indexToUriCache) {
			m_defaultServiceSettings.callTimeout = 10000;
			m_opcUaWrapper = std::move(opcUaWrapper);
			m_opcUaWrapper->setSubscription(&m_subscr);

			if (++PlatformLayerInitialized == 1) {
				UaPlatformLayer::init();
			}

			m_tryConnecting = true;
			// Try connecting at least once
			this->connect();
			m_connectThread = std::make_shared<std::thread>([this]() { this->threadConnectExecution(); });

		}

		bool OpcUaClient::connect() {
			UaString sURL(m_serverUri.c_str());
			UaStatus result;

			UaClientSdk::SessionSecurityInfo sessionSecurityInfo;
			UaClientSdk::ServiceSettings serviceSettings;
			UaEndpointDescriptions endpointDescriptions;
			UaApplicationDescriptions applicationDescriptions;
			SetupSecurity::setupSecurity(&sessionSecurityInfo);

			result = m_opcUaWrapper->DiscoveryGetEndpoints(serviceSettings, sURL, sessionSecurityInfo,
														   endpointDescriptions);
			if (result.isBad()) {
				LOG(ERROR) << result.toString().toUtf8();
				return false;
			}

			struct {
				UaString url;
				UaByteString serverCertificate;
				UaString securityPolicy;
				OpcUa_UInt32 securityMode{};
			} desiredEndpoint;


			auto desiredSecurity = m_security;
			for (OpcUa_UInt32 iEndpoint = 0; iEndpoint < endpointDescriptions.length(); iEndpoint++) {

				if (endpointDescriptions[iEndpoint].SecurityMode != desiredSecurity) {
					continue;
				}
				desiredEndpoint.url = UaString(endpointDescriptions[iEndpoint].EndpointUrl);
				LOG(INFO) << "desiredEndpoint.url: " << desiredEndpoint.url.toUtf8() << std::endl;
				sessionSecurityInfo.serverCertificate = endpointDescriptions[iEndpoint].ServerCertificate;
				sessionSecurityInfo.sSecurityPolicy = endpointDescriptions[iEndpoint].SecurityPolicyUri;
				sessionSecurityInfo.messageSecurityMode = static_cast<OpcUa_MessageSecurityMode>(endpointDescriptions[iEndpoint].SecurityMode);
				break;
			}

			if (desiredEndpoint.url.isEmpty()) {
				LOG(ERROR) << "Could not find endpoint without encryption." << std::endl;
				return false;
			}

			///\todo handle security
			sessionSecurityInfo.doServerCertificateVerify = OpcUa_False;
			sessionSecurityInfo.disableErrorCertificateHostNameInvalid = OpcUa_True;
			sessionSecurityInfo.disableApplicationUriCheck = OpcUa_True;

			if (!m_username.empty() && !m_password.empty()) {
				sessionSecurityInfo.setUserPasswordUserIdentity(m_username.c_str(), m_password.c_str());
			}

			m_opcUaWrapper->GetNewSession(m_pSession);

			UaClientSdk::SessionConnectInfo sessionConnectInfo;
			sessionConnectInfo = prepareSessionConnectInfo(sessionConnectInfo);

			result = m_opcUaWrapper->SessionConnect(sURL, sessionConnectInfo, sessionSecurityInfo, this);

			if (!result.isGood()) {
				LOG(ERROR) << "Connecting failed in OPC UA Data Client: " << result.toString().toUtf8() << std::endl;
				return false;
			}

			return true;
		}

		UaClientSdk::SessionConnectInfo &
		OpcUaClient::prepareSessionConnectInfo(UaClientSdk::SessionConnectInfo &sessionConnectInfo) {
			sessionConnectInfo.sApplicationName = "KonI4.0 OPC UA Data Client";
			sessionConnectInfo.sApplicationUri = "http://dashboard.umati.app/OPCUA_DataClient";
			sessionConnectInfo.sProductUri = "KonI40OpcUaClient_Product";
			sessionConnectInfo.sSessionName = "DefaultSession";
			return sessionConnectInfo;
		}

		void OpcUaClient::on_connected() {
			updateNamespaceCache();
			m_opcUaWrapper->SubscriptionCreateSubscription(m_pSession);
		}

		std::string OpcUaClient::getTypeName(const ModelOpcUa::NodeId_t &nodeId) {
			return readNodeBrowseName(nodeId);
		}

		std::string OpcUaClient::readNodeBrowseName(const ModelOpcUa::NodeId_t &_nodeId) {
			auto nodeId = Converter::ModelNodeIdToUaNodeId(_nodeId, m_uriToIndexCache).getNodeId();

			checkConnection();

			UaReadValueIds readValueIds;
			readValueIds.create(1);
			nodeId.copyTo(&readValueIds[0].NodeId);
			readValueIds[0].AttributeId = OpcUa_Attributes_BrowseName;

			UaDataValues readResult;

			UaDiagnosticInfos diagInfo;

			auto uaResult = m_opcUaWrapper->SessionRead(
					m_defaultServiceSettings,
					100.0,
					OpcUa_TimestampsToReturn_Neither,
					readValueIds,
					readResult,
					diagInfo
			);

			if (uaResult.isBad()) {
				LOG(ERROR) << "readNodeClass failed for node: '" << nodeId.toXmlString().toUtf8()
						   << "' with " << uaResult.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			if (readResult.length() != 1) {
				LOG(ERROR) << "readResult.length() expect 1  got:" << readResult.length();
				throw Exceptions::UmatiException("Length mismatch");
			}

			UaStatusCode uaResultElement(readResult[0].StatusCode);
			if (uaResultElement.isBad()) {
				LOG(WARNING) << "Bad value status code failed for node: '" << nodeId.toFullString().toUtf8()
							 << "' with " << uaResultElement.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement);
			}

			UaVariant value(readResult[0].Value);
			if (value.type() != OpcUaType_QualifiedName) {
				LOG(ERROR) << "Expect Type Int32, got '" << value.type();
				throw Exceptions::UmatiException("Type mismatch");
			}

			return _nodeId.Uri + ";" + value.toString().toUtf8();
		}

		OpcUa_NodeClass OpcUaClient::readNodeClass(const UaNodeId &nodeId) {
			checkConnection();

			UaReadValueIds readValueIds;
			readValueIds.create(1);
			nodeId.copyTo(&readValueIds[0].NodeId);
			readValueIds[0].AttributeId = OpcUa_Attributes_NodeClass;

			UaDataValues readResult;

			UaDiagnosticInfos diagInfo;

			auto uaResult = m_opcUaWrapper->SessionRead(
					m_defaultServiceSettings,
					100.0,
					OpcUa_TimestampsToReturn_Neither,
					readValueIds,
					readResult,
					diagInfo
			);

			if (uaResult.isBad()) {
				LOG(ERROR) << "readNodeClass failed for node: '" << nodeId.toXmlString().toUtf8()
						   << "' with " << uaResult.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			if (readResult.length() != 1) {
				LOG(ERROR) << "readResult.length() expect 1  got:" << readResult.length();
				throw Exceptions::UmatiException("Length mismatch");
			}

			UaStatusCode uaResultElement(readResult[0].StatusCode);
			if (uaResultElement.isBad()) {
				LOG(WARNING) << "Bad value status code failed for node: '" << nodeId.toFullString().toUtf8()
							 << "' with " << uaResultElement.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement);
			}

			UaVariant value(readResult[0].Value);
			if (value.type() != OpcUaType_Int32) {
				LOG(ERROR) << "Expect Type Int32, got '" << value.type();
				throw Exceptions::UmatiException("Type mismatch");
			}

			OpcUa_Int32 nodeClass;
			value.toInt32(nodeClass);

			return static_cast<OpcUa_NodeClass>(nodeClass);
		}

		void OpcUaClient::checkConnection() {
			if (!this->m_isConnected || !m_opcUaWrapper->SessionIsConnected()) {
				throw Exceptions::ClientNotConnected("Need connected client.");
			}
		}

		UaNodeId OpcUaClient::browseSuperType(const UaNodeId &typeNodeId) {
			checkConnection();

			auto referenceTypeUaNodeId = UaNodeId(OpcUaId_HasSubtype);


			UaClientSdk::BrowseContext browseContext;
			browseContext.browseDirection = OpcUa_BrowseDirection_Inverse;
			browseContext.includeSubtype = OpcUa_True;
			browseContext.maxReferencesToReturn = 0;
			browseContext.nodeClassMask = 0; // ALL
			browseContext.referenceTypeId = referenceTypeUaNodeId;
			browseContext.resultMask = OpcUa_BrowseResultMask_None;

			OpcUa_NodeClass nodeClass = readNodeClass(typeNodeId);

			switch (nodeClass) {
				case OpcUa_NodeClass_ObjectType:
				case OpcUa_NodeClass_VariableType: {
					browseContext.nodeClassMask = nodeClass;
					break;
				}
				default:
					LOG(ERROR) << "Invalid NodeClass " << nodeClass;
					throw Exceptions::UmatiException("Invalid NodeClass");
			}

			UaByteString continuationPoint;
			UaReferenceDescriptions referenceDescriptions;
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, typeNodeId, browseContext,
														  continuationPoint, referenceDescriptions);

			if (uaResult.isBad()) {
				LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			std::list<ModelOpcUa::BrowseResult_t> browseResult;


			if (referenceDescriptions.length() == 0) {
				return UaNodeId();
			}

			if (referenceDescriptions.length() > 1) {
				LOG(ERROR) << "Found multiple superTypes for " << typeNodeId.toXmlString().toUtf8();
				return UaNodeId();
			}

			return UaNodeId(UaExpandedNodeId(referenceDescriptions[0].NodeId).nodeId());
		}

		bool
		OpcUaClient::isSameOrSubtype(const UaNodeId &expectedType, const UaNodeId &checkType, std::size_t maxDepth) {
			if (checkType.isNull()) {
				return false;
			}

			if (expectedType == checkType) {
				return true;
			}

			auto it = m_superTypes.find(checkType);

			if (it != m_superTypes.end()) {
				return isSameOrSubtype(expectedType, it->second, --maxDepth);
			}

			auto superType = browseSuperType(checkType);
			m_superTypes[checkType] = superType;
			return isSameOrSubtype(expectedType, superType, --maxDepth);
		}

		void OpcUaClient::updateNamespaceCache() {
			std::vector<std::string> notFoundObjectTypeNamespaces;
			UaStringArray uaNamespaces = m_opcUaWrapper->SessionGetNamespaceTable();

			initializeNamespaceCache(notFoundObjectTypeNamespaces);

			fillNamespaceCache(uaNamespaces);

			std::shared_ptr<std::map<std::string, std::shared_ptr<
					ModelOpcUa::StructureBiNode>>> bidirectionalTypeMap =
					std::make_shared<
							std::map<std::string,
									std::shared_ptr<ModelOpcUa::StructureBiNode>>>();

			browseObjectOrVariableTypeAndFillBidirectionalTypeMap(m_basicVariableTypeNode, bidirectionalTypeMap, true);
			LOG(INFO) << "Browsing variable types finished, continuing browsing object types";

			browseObjectOrVariableTypeAndFillBidirectionalTypeMap(m_basicObjectTypeNode, bidirectionalTypeMap, false);
			LOG(INFO) << "Browsing object types finished";

			for (std::size_t i = 0; i < uaNamespaces.length(); ++i) {
				auto uaNamespace = uaNamespaces[i];
				auto uaNamespaceAsUaString = UaString(uaNamespace);
				auto uaNamespaceUtf8 = uaNamespaceAsUaString.toUtf8();
				std::string namespaceURI(uaNamespaceUtf8);
				findObjectTypeNamespacesAndCreateTypeMap(notFoundObjectTypeNamespaces, i, namespaceURI,
														 bidirectionalTypeMap);
			}

			for (auto &notFoundObjectTypeNamespace : notFoundObjectTypeNamespaces) {
				LOG(WARNING) << "Unable to find namespace " << notFoundObjectTypeNamespace;
			}

			updateTypeMap();
		}

		void OpcUaClient::updateTypeMap() {
			for (auto mapIterator = m_typeMap->begin(); mapIterator != m_typeMap->end(); mapIterator++) {
				for (auto childIterator = mapIterator->second->SpecifiedChildNodes->begin();
					 childIterator != mapIterator->second->SpecifiedChildNodes->end(); childIterator++) {
					try {
						std::string childTypeName = getTypeName(childIterator->get()->SpecifiedTypeNodeId);
						auto childType = m_typeMap->find(childTypeName);

						if (childType != m_typeMap->end()) {

							childIterator->get()->SpecifiedChildNodes = childType->second->SpecifiedChildNodes;
							childIterator->get()->ofBaseDataVariableType = childType->second->ofBaseDataVariableType;
							//LOG(INFO) << "Updating type " << childTypeName <<" for " << childIterator->get()->SpecifiedBrowseName.Uri << ";" << childIterator->get()->SpecifiedBrowseName.Name;
						}
					} catch (std::exception &ex) {
						LOG(WARNING) << "Unable to update type due to " << ex.what();
					}
				}
			}
			LOG(INFO) << "Updated typeMap";
		}

		void OpcUaClient::initializeNamespaceCache(std::vector<std::string> &notFoundObjectTypeNamespaces) {
			m_opcUaWrapper->SessionUpdateNamespaceTable();

			m_uriToIndexCache.clear();
			m_indexToUriCache.clear();
			m_availableObjectTypeNamespaces.clear();

			for (auto &m_expectedObjectTypeNamespace : m_expectedObjectTypeNamespaces) {
				notFoundObjectTypeNamespaces.push_back(m_expectedObjectTypeNamespace);
			}
		}

		void OpcUaClient::fillNamespaceCache(const UaStringArray &uaNamespaces) {
			for (std::size_t i = 0; i < uaNamespaces.length(); ++i) {
				auto uaNamespace = uaNamespaces[i];
				auto uaNamespaceAsUaString = UaString(uaNamespace);
				auto uaNamespaceUtf8 = uaNamespaceAsUaString.toUtf8();
				std::string namespaceURI(uaNamespaceUtf8);
				m_uriToIndexCache[namespaceURI] = static_cast<uint16_t>(i);
				m_indexToUriCache[static_cast<uint16_t>(i)] = namespaceURI;
				LOG(INFO) << "index: " << std::to_string(i) << ", namespaceURI: " << namespaceURI;
			}
		}

		void
		OpcUaClient::browseObjectOrVariableTypeAndFillBidirectionalTypeMap(const ModelOpcUa::NodeId_t &basicTypeNode,
																		   std::shared_ptr<std::map<std::string,
																				   std::shared_ptr<
																						   ModelOpcUa::StructureBiNode>>

																		   > bidirectionalTypeMap,
																		   bool ofBaseDataVariableType
		) {
			// startBrowseTypeResult is needed to create a startVariableType
			const ModelOpcUa::BrowseResult_t startBrowseTypeResult{
					ModelOpcUa::NodeClass_t::VariableType, basicTypeNode, m_emptyId, m_emptyId,
					ModelOpcUa::QualifiedName_t{basicTypeNode.Uri, ""} // BrowseName
			};
			auto startVariableType = handleBrowseTypeResult(bidirectionalTypeMap, startBrowseTypeResult, nullptr,
															ModelOpcUa::ModellingRule_t::Mandatory,
															ofBaseDataVariableType);

			UaClientSdk::BrowseContext browseTypeContext = prepareObjectAndVariableTypeBrowseContext();
			UaNodeId startUaNodeId = Converter::ModelNodeIdToUaNodeId(basicTypeNode, m_uriToIndexCache).getNodeId();
			browseTypes(bidirectionalTypeMap, browseTypeContext, startUaNodeId, startVariableType,
						ofBaseDataVariableType
			);
		}

		uint OpcUaClient::GetImplementedNamespaceIndex(const ModelOpcUa::NodeId_t &nodeId) {
			UaReferenceDescriptions machineTypeDefinitionReferenceDescriptions;
			auto startFromMachineNodeId = UaNodeId::fromXmlString(UaString(nodeId.Id.c_str()));
			uint machineNamespaceIndex = m_uriToIndexCache[nodeId.Uri];
			startFromMachineNodeId.setNamespaceIndex(machineNamespaceIndex);

			UaClientSdk::BrowseContext browseContext;
			browseContext.referenceTypeId = OpcUaId_HasTypeDefinition;
			browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
			browseContext.includeSubtype = OpcUa_True;
			browseContext.maxReferencesToReturn = 0;
			browseContext.nodeClassMask = 0; // ALL
			browseContext.resultMask = OpcUa_BrowseResultMask_All;
			browseUnderStartNode(startFromMachineNodeId, machineTypeDefinitionReferenceDescriptions,
								 browseContext);

			uint machineTypeNamespaceIndex = 0;
			for (OpcUa_UInt32 i = 0; i < machineTypeDefinitionReferenceDescriptions.length(); i++) {
				machineTypeNamespaceIndex = machineTypeDefinitionReferenceDescriptions[i].NodeId.NodeId.NamespaceIndex;
			}
			return machineTypeNamespaceIndex;
		}

		void
		OpcUaClient::CreateMachineListForNamespaceUnderStartNode(std::list<ModelOpcUa::BrowseResult_t> &machineList,
																 const std::string &startNodeNamespaceUri,
																 const ModelOpcUa::NodeId_t &startNode) {
			auto startNodeId = UaNodeId::fromXmlString(UaString(startNode.Id.c_str()));
			UaReferenceDescriptions referenceDescriptions;
			uint namespaceIndex = m_uriToIndexCache[startNodeNamespaceUri];
			startNodeId.setNamespaceIndex(namespaceIndex);

			browseUnderStartNode(startNodeId, referenceDescriptions);

			for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++) {
				auto refDesc = referenceDescriptions[i];
				try {
					auto browseRes = ReferenceDescriptionToBrowseResult(refDesc);
					machineList.emplace_back(browseRes);
				} catch (std::exception &ex) { LOG(ERROR) << "unable to add machine: " << ex.what(); }
			}
		}

		void
		OpcUaClient::FillIdentificationValuesFromBrowseResult(std::list<ModelOpcUa::BrowseResult_t> &identification,
															  std::list<ModelOpcUa::NodeId_t> &identificationNodes,
															  std::vector<std::string> &identificationValueKeys) {
			auto startNodeId = UaNodeId::fromXmlString(UaString(identification.front().NodeId.Id.c_str()));
			startNodeId.setNamespaceIndex(m_uriToIndexCache[identification.front().NodeId.Uri]);

			UaReferenceDescriptions referenceDescriptions;
			browseUnderStartNode(startNodeId, referenceDescriptions);
			for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++) {
				ModelOpcUa::BrowseResult_t browseResult = ReferenceDescriptionToBrowseResult(
						referenceDescriptions[i]);
				identificationValueKeys.push_back(browseResult.BrowseName.Name);
				identificationNodes.emplace_back(browseResult.NodeId);
			}
		}

		void
		OpcUaClient::findObjectTypeNamespacesAndCreateTypeMap(std::vector<std::string> &notFoundObjectTypeNamespaces,
															  size_t i,
															  const std::string &namespaceURI,
															  std::shared_ptr<std::map<std::string,
																	  std::shared_ptr<ModelOpcUa::StructureBiNode>>

															  > bidirectionalTypeMap) {
			auto it = find(notFoundObjectTypeNamespaces.begin(), notFoundObjectTypeNamespaces.end(), namespaceURI);
			if (it != notFoundObjectTypeNamespaces.

					end()

					) {
				NamespaceInformation_t information;

				std::vector<std::string> resultContainer;
				split(namespaceURI, resultContainer,
					  '/');

				information.
						Namespace = resultContainer.back();
				information.
						NamespaceUri = namespaceURI;

				std::string typeName;
				std::string identificationTypeName;

				UaReferenceDescriptions referenceDescriptions;
				std::string startNodeNamespaceUri = "http://opcfoundation.org/UA/Machinery/";
				ModelOpcUa::NodeId_t startNode = ModelOpcUa::NodeId_t{startNodeNamespaceUri, "i=1012"};
				auto startNodeId = UaNodeId::fromXmlString(UaString(startNode.Id.c_str()));
				uint namespaceIndex = m_uriToIndexCache[startNodeNamespaceUri];
				startNodeId.
						setNamespaceIndex(namespaceIndex);
				browseUnderStartNode(startNodeId, referenceDescriptions
				);

				if (referenceDescriptions.

						length()

					> 0) {
					for (
							OpcUa_UInt32 j = 0;
							j < referenceDescriptions.

									length();

							j++) {
						if (referenceDescriptions[j].BrowseName.NamespaceIndex == i) {
							identificationTypeName = UaString(referenceDescriptions[j].BrowseName.Name).toUtf8();
							break;
						}
					}
				}
				if (identificationTypeName.

						empty()

						) {
					identificationTypeName = information.Namespace + "IdentificationType";
				}

				UaReferenceDescriptions referenceDescriptions2;
				auto startNode2 = ModelOpcUa::NodeId_t{"http://opcfoundation.org/UA/", "i=58"};
				auto startNodeId2 = UaNodeId::fromXmlString(UaString(startNode2.Id.c_str()));
				uint namespaceIndex2 = m_uriToIndexCache[startNodeNamespaceUri];
				startNodeId.
						setNamespaceIndex(namespaceIndex2);
				browseUnderStartNode(startNodeId2, referenceDescriptions2
				);
				if (referenceDescriptions2.

						length()

					> 0) {
					for (
							OpcUa_UInt32 j = 0;
							j < referenceDescriptions2.

									length();

							j++) {
						if (referenceDescriptions2[j].BrowseName.NamespaceIndex ==
							i && referenceDescriptions2[j]
										 .NodeId.NodeId.Identifier.Numeric == 1014) {
							typeName = UaString(referenceDescriptions2[j].BrowseName.Name).toUtf8();
							break;
						}
					}
				}
				if (typeName.

						empty()

						) {
					typeName = information.Namespace + "Type";
				}

				information.
						NamespaceType = typeName;
				information.
						NamespaceIdentificationType = identificationTypeName;

				m_availableObjectTypeNamespaces[static_cast
						<uint16_t>(i)
				] =
						information;
				notFoundObjectTypeNamespaces.
						erase(it);
				LOG(INFO)
						<< "Expected object type namespace " << namespaceURI << " found at index " <<
						std::to_string(i);
				createTypeMap(bidirectionalTypeMap, m_typeMap, i
				);
				LOG(INFO)
						<< "Finished creatingTypeMap for " <<
						namespaceURI;
			}
		}

		void OpcUaClient::browseUnderStartNode(const UaNodeId &startUaNodeId,
											   UaReferenceDescriptions &referenceDescriptions) {
			browseUnderStartNode(startUaNodeId, referenceDescriptions, prepareObjectAndVariableTypeBrowseContext());
		}

		void
		OpcUaClient::browseUnderStartNode(const UaNodeId &startUaNodeId, UaReferenceDescriptions &referenceDescriptions,
										  const UaClientSdk::BrowseContext &browseContext) {
			UaByteString continuationPoint;
			// References -> nodes referenced to this, e.g. child nodes
			// BrowseName: Readable Name and namespace index
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, startUaNodeId, browseContext,
														  continuationPoint, referenceDescriptions);
			if (uaResult.isBad()) {
				LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8() << " for node "
						   << startUaNodeId.toFullString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}
		}

		void OpcUaClient::browseTypes(std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode
		>>> bidirectionalTypeMap,
									  const UaClientSdk::BrowseContext &browseContext,
									  const UaNodeId &startUaNodeId,
									  const std::shared_ptr<ModelOpcUa::StructureBiNode> &parent,
									  bool ofBaseDataVariableType
		) {

			UaReferenceDescriptions referenceDescriptions;
			browseUnderStartNode(startUaNodeId, referenceDescriptions, browseContext
			);

			for (
					OpcUa_UInt32 i = 0;
					i < referenceDescriptions.

							length();

					i++) {
				ModelOpcUa::BrowseResult_t browseResult = ReferenceDescriptionToBrowseResult(referenceDescriptions[i]);
				UaNodeId nextUaNodeId = Converter::ModelNodeIdToUaNodeId(browseResult.NodeId,
																		 m_uriToIndexCache).getNodeId();
				ModelOpcUa::ModellingRule_t modellingRule = browseModellingRule(nextUaNodeId);
// LOG(INFO) << "currently at " << startUaNodeId.toFullString().toUtf8();
				auto current = handleBrowseTypeResult(bidirectionalTypeMap, browseResult, parent, modellingRule,
													  ofBaseDataVariableType);
				browseTypes(bidirectionalTypeMap, browseContext, nextUaNodeId, current, ofBaseDataVariableType
				);
			}
		}

		ModelOpcUa::ModellingRule_t OpcUaClient::browseModellingRule(const UaNodeId &uaNodeId) {
			UaByteString continuationPoint;
			UaReferenceDescriptions referenceDescriptions;

			/// begin browse modelling rule
			UaClientSdk::BrowseContext browseContext2 = prepareObjectAndVariableTypeBrowseContext();
			browseContext2.referenceTypeId = UaNodeId(OpcUaId_HasModellingRule);
			ModelOpcUa::ModellingRule_t modellingRule = ModelOpcUa::ModellingRule_t::Optional;

			auto uaResult2 = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, uaNodeId,
														   browseContext2,
														   continuationPoint, referenceDescriptions);
			if (uaResult2.isBad()) {
				LOG(ERROR) << "Bad return from browse: " << uaResult2.toString().toUtf8() << "for nodeId"
						   << uaNodeId.toFullString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult2);
			}
			for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++) {
				auto refDescr = referenceDescriptions[i];
				ModelOpcUa::BrowseResult_t browseResult = ReferenceDescriptionToBrowseResult(refDescr);
				if (browseResult.BrowseName.Name == "Mandatory") {
					modellingRule = ModelOpcUa::Mandatory;
				} else if (browseResult.BrowseName.Name == "Optional") {
					modellingRule = ModelOpcUa::Optional;
				} else if (browseResult.BrowseName.Name == "MandatoryPlaceholder") {
					modellingRule = ModelOpcUa::MandatoryPlaceholder;
				} else if (browseResult.BrowseName.Name == "OptionalPlaceholder") {
					modellingRule = ModelOpcUa::OptionalPlaceholder;
				}
			}
			return modellingRule;
		}

		std::shared_ptr<ModelOpcUa::StructureBiNode> OpcUaClient::handleBrowseTypeResult(
				std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode
				>>> &bidirectionalTypeMap,
				const ModelOpcUa::BrowseResult_t &entry,
				const std::shared_ptr<ModelOpcUa::StructureBiNode> &parent, ModelOpcUa::ModellingRule_t
				modellingRule,
				bool ofBaseDataVariableType
		) {
			UaNodeId currentUaNodeId = Converter::ModelNodeIdToUaNodeId(entry.NodeId, m_uriToIndexCache).getNodeId();

			bool isObjectType = ModelOpcUa::ObjectType == entry.NodeClass;
			bool isVariableType = ModelOpcUa::VariableType == entry.NodeClass;
			ModelOpcUa::StructureBiNode node(entry, ofBaseDataVariableType,
											 std::make_shared<std::list<std::shared_ptr<ModelOpcUa::StructureNode
											 >>>(), parent, (uint16_t) currentUaNodeId.

							namespaceIndex(), modellingRule

			);
			auto current = std::make_shared<ModelOpcUa::StructureBiNode>(node);

			if (isObjectType || isVariableType) {
				std::string typeName = node.structureNode->SpecifiedBrowseName.Uri + ";" +
									   node.structureNode->SpecifiedBrowseName.Name;
				if (bidirectionalTypeMap->
						count(typeName)
					== 0) {
					current->
							isType = true;
					current->
							ofBaseDataVariableType = ofBaseDataVariableType;
					std::pair<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>> newType(typeName, current);
					bidirectionalTypeMap->
							insert(newType);
					std::pair<std::string, ModelOpcUa::NodeId_t> newNameMapping(typeName, entry.NodeId);
					m_nameToId->
							insert(newNameMapping);
					if ((bidirectionalTypeMap->

							size()

						 % 50) == 0) {
						LOG(INFO)
								<< "Current size BiDirectionalTypeMap: " << bidirectionalTypeMap->

								size();

					}
				} else {
					LOG(INFO)
							<< "Found Type " << typeName << " again";
				}
			}
			if (parent != nullptr) {
				parent->SpecifiedBiChildNodes->
						emplace_back(current);
			}
			return
					current;
		}

		UaClientSdk::BrowseContext OpcUaClient::prepareObjectAndVariableTypeBrowseContext() {
			UaClientSdk::BrowseContext browseContext;
			browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
			browseContext.includeSubtype = OpcUa_True;
			browseContext.maxReferencesToReturn = 0;
			browseContext.resultMask = OpcUa_BrowseResultMask_All;
			browseContext.nodeClassMask =
					OpcUa_NodeClass_Object
					+ OpcUa_NodeClass_Variable
					+ OpcUa_NodeClass_ObjectType
					+ OpcUa_NodeClass_VariableType;
			/*
			 * - OpcUa_NodeClass_Object        = 1,
			 * - OpcUa_NodeClass_Variable      = 2,
			 * - OpcUa_NodeClass_Method        = 4,
			 * - OpcUa_NodeClass_ObjectType    = 8,
			 * - OpcUa_NodeClass_VariableType  = 16,
			 * - OpcUa_NodeClass_ReferenceType = 32,
			 * - OpcUa_NodeClass_DataType      = 64,
			 * - OpcUa_NodeClass_View          = 128
			 * */
			return browseContext;
		}

		void OpcUaClient::connectionStatusChanged(OpcUa_UInt32 /*clientConnectionId*/,
												  UaClientSdk::UaClient::ServerStatus serverStatus) {
			switch (serverStatus) {
				case UaClientSdk::UaClient::Disconnected:
					LOG(ERROR) << "Disconnected." << std::endl;
					m_isConnected = false;
					break;
				case UaClientSdk::UaClient::Connected:
					LOG(ERROR) << "Connected." << std::endl;
					m_isConnected = true;
					on_connected();
					break;
				case UaClientSdk::UaClient::ConnectionWarningWatchdogTimeout:
					LOG(ERROR) << "ConnectionWarningWatchdogTimeout." << std::endl;
					break;
				case UaClientSdk::UaClient::ConnectionErrorApiReconnect:
					LOG(ERROR) << "ConnectionErrorApiReconnect." << std::endl;
					break;
				case UaClientSdk::UaClient::ServerShutdown:
					LOG(ERROR) << "ServerShutdown." << std::endl;
					break;
				case UaClientSdk::UaClient::NewSessionCreated:
					LOG(ERROR) << "NewSessionCreated." << std::endl;
					break;
			}
		}

		OpcUaClient::~OpcUaClient() {
			// Destroy all sessions before UaPlatformLayer::cleanup(); is called!
			m_tryConnecting = false;
			if (m_connectThread) {
				m_connectThread->join();
			}

			m_pSession = nullptr;

			if (--PlatformLayerInitialized == 0) {
				UaPlatformLayer::cleanup();
			}
		}

		bool OpcUaClient::disconnect() {
			if (m_pSession) {
				//Subscr.deleteSubscription(m_pSession);
				UaClientSdk::ServiceSettings servsettings;
				return m_opcUaWrapper->SessionDisconnect(servsettings, OpcUa_True).isGood() != OpcUa_False;
			}

			return true;
		}

		void OpcUaClient::threadConnectExecution() {
			while (m_tryConnecting) {
				if (!m_isConnected) {
					this->connect();
				} else {
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
		}

		std::list<ModelOpcUa::BrowseResult_t> OpcUaClient::BrowseHasComponent(ModelOpcUa::NodeId_t startNode,
																			  ModelOpcUa::NodeId_t typeDefinition) {
			auto hasComponents = ModelOpcUa::NodeId_t{"", std::to_string(OpcUaId_HasComponent)};
			return Browse(startNode, hasComponents, typeDefinition);
		}

		std::list<ModelOpcUa::BrowseResult_t> OpcUaClient::Browse(
				ModelOpcUa::NodeId_t startNode,
				ModelOpcUa::NodeId_t referenceTypeId,
				ModelOpcUa::NodeId_t typeDefinition) {
			UaClientSdk::BrowseContext browseContext = prepareBrowseContext(referenceTypeId);
			return BrowseWithContext(startNode, referenceTypeId, typeDefinition, browseContext);
		}

		std::list<ModelOpcUa::BrowseResult_t>
		OpcUaClient::BrowseWithContext(const ModelOpcUa::NodeId_t &startNode,
									   const ModelOpcUa::NodeId_t &referenceTypeId,
									   const ModelOpcUa::NodeId_t &typeDefinition,
									   UaClientSdk::BrowseContext &browseContext) {
			checkConnection();
			auto startUaNodeId = Converter::ModelNodeIdToUaNodeId(startNode, m_uriToIndexCache).getNodeId();
			auto referenceTypeUaNodeId = Converter::ModelNodeIdToUaNodeId(referenceTypeId,
																		  m_uriToIndexCache).getNodeId();
			auto typeDefinitionUaNodeId = Converter::ModelNodeIdToUaNodeId(typeDefinition,
																		   m_uriToIndexCache).getNodeId();


			OpcUa_NodeClass nodeClass = readNodeClass(typeDefinitionUaNodeId);

			switch (nodeClass) {
				case OpcUa_NodeClass_ObjectType: {
					browseContext.nodeClassMask = OpcUa_NodeClass_Object;
					break;
				}
				case OpcUa_NodeClass_VariableType: {
					browseContext.nodeClassMask = OpcUa_NodeClass_Variable;
					break;
				}
				default:
					LOG(ERROR) << "Invalid NodeClass " << nodeClass
							   << " expect object or variable type for node "
							   << static_cast<std::string>(typeDefinition);
					throw Exceptions::UmatiException("Invalid NodeClass");
			}

			UaByteString continuationPoint;
			UaReferenceDescriptions referenceDescriptions;
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, startUaNodeId, browseContext,
														  continuationPoint, referenceDescriptions);

			if (uaResult.isBad()) {
				LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8() << ", with startUaNodeId "
						   << startUaNodeId.toFullString().toUtf8()
						   << " and ref id " << browseContext.referenceTypeId.toFullString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			std::list<ModelOpcUa::BrowseResult_t> browseResult;
			ReferenceDescriptionsToBrowseResults(typeDefinitionUaNodeId, referenceDescriptions, browseResult);
			handleContinuationPoint(continuationPoint);

			return browseResult;
		}

		void OpcUaClient::ReferenceDescriptionsToBrowseResults(const UaNodeId &typeDefinitionUaNodeId,
															   const UaReferenceDescriptions &referenceDescriptions,
															   std::list<ModelOpcUa::BrowseResult_t> &browseResult) {
			for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++) {
				auto browseTypeNodeId = UaNodeId(UaExpandedNodeId(referenceDescriptions[i].TypeDefinition).nodeId());
				if (!isSameOrSubtype(typeDefinitionUaNodeId, browseTypeNodeId)) {
					continue;
				}

				auto entry = ReferenceDescriptionToBrowseResult(referenceDescriptions[i]);

				browseResult.push_back(entry);
			}
		}

		void OpcUaClient::handleContinuationPoint(const UaByteString &/*continuationPoint*/) {
			LOG(DEBUG) << "Handling continuation point not yet implemented";
		}


		ModelOpcUa::BrowseResult_t
		OpcUaClient::ReferenceDescriptionToBrowseResult(const OpcUa_ReferenceDescription &referenceDescription) {
			ModelOpcUa::BrowseResult_t entry;
			auto browseTypeUaNodeId = UaNodeId(UaExpandedNodeId(referenceDescription.TypeDefinition).nodeId());
			entry.NodeClass = Converter::UaNodeClassToModelNodeClass(referenceDescription.NodeClass).getNodeClass();
			entry.TypeDefinition = Converter::UaNodeIdToModelNodeId(browseTypeUaNodeId, m_indexToUriCache).getNodeId();
			entry.NodeId = Converter::UaNodeIdToModelNodeId(
					UaNodeId(UaExpandedNodeId(referenceDescription.NodeId).nodeId()),
					m_indexToUriCache).getNodeId();
			auto referenceTypeUaNodeId = UaNodeId(referenceDescription.ReferenceTypeId);
			auto referenceTypeModelNodeId = Converter::UaNodeIdToModelNodeId(referenceTypeUaNodeId, m_indexToUriCache);
			entry.ReferenceTypeId = referenceTypeModelNodeId.getNodeId();
			entry.BrowseName = Converter::UaQualifiedNameToModelQualifiedName(referenceDescription.BrowseName,
																			  m_indexToUriCache).getQualifiedName();

			return entry;
		}

		UaClientSdk::BrowseContext OpcUaClient::prepareBrowseContext(ModelOpcUa::NodeId_t referenceTypeId) {
			auto referenceTypeUaNodeId = Converter::ModelNodeIdToUaNodeId(std::move(referenceTypeId),
																		  m_uriToIndexCache).getNodeId();
			UaClientSdk::BrowseContext browseContext;
			browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
			browseContext.includeSubtype = OpcUa_True;
			browseContext.maxReferencesToReturn = 0;
			browseContext.nodeClassMask = 0; // ALL
			if (nullptr != referenceTypeUaNodeId) {
				browseContext.referenceTypeId = referenceTypeUaNodeId;
			}
			browseContext.resultMask =
					OpcUa_BrowseResultMask_BrowseName
					+ OpcUa_BrowseResultMask_TypeDefinition
					+ OpcUa_BrowseResultMask_NodeClass
					+ OpcUa_BrowseResultMask_ReferenceTypeId;
			return browseContext;
		}

		ModelOpcUa::NodeId_t
		OpcUaClient::TranslateBrowsePathToNodeId(ModelOpcUa::NodeId_t startNode,
												 ModelOpcUa::QualifiedName_t browseName) {
			checkConnection();

			if (browseName.isNull()) {
				LOG(ERROR) << "browseName is NULL";
				throw std::invalid_argument("browseName is NULL");
			}

			if (startNode.isNull()) {
				LOG(ERROR) << "startNode is NULL";
				throw std::invalid_argument("startNode is NULL");
			}

			auto startUaNodeId = Converter::ModelNodeIdToUaNodeId(startNode, m_uriToIndexCache).getNodeId();
			auto uaBrowseName = Converter::ModelQualifiedNameToUaQualifiedName(browseName,
																			   m_uriToIndexCache).getQualifiedName();
			// LOG(INFO) << "translateBrowsePathToNodeId: start from " << startUaNodeId.toString().toUtf8() << " and search " << uaBrowseName.toString().toUtf8();

			UaRelativePathElements uaBrowsePathElements;
			uaBrowsePathElements.create(1);
			uaBrowsePathElements[0].IncludeSubtypes = OpcUa_True;
			uaBrowsePathElements[0].IsInverse = OpcUa_False;
			uaBrowsePathElements[0].ReferenceTypeId.Identifier.Numeric = OpcUaId_HierarchicalReferences;
			uaBrowseName.copyTo(&uaBrowsePathElements[0].TargetName);

			UaBrowsePaths uaBrowsePaths;
			uaBrowsePaths.create(1);
			uaBrowsePaths[0].RelativePath.NoOfElements = uaBrowsePathElements.length();
			uaBrowsePaths[0].RelativePath.Elements = uaBrowsePathElements.detach();
			startUaNodeId.copyTo(&uaBrowsePaths[0].StartingNode);

			UaBrowsePathResults uaBrowsePathResults;
			UaDiagnosticInfos uaDiagnosticInfos;

			auto uaResult = m_opcUaWrapper->SessionTranslateBrowsePathsToNodeIds(
					m_defaultServiceSettings,
					uaBrowsePaths,
					uaBrowsePathResults,
					uaDiagnosticInfos
			);

			if (uaResult.isBad()) {
				LOG(ERROR) << "TranslateBrowsePathToNodeId failed for node: '" << static_cast<std::string>(startNode)
						   << "' with " << uaResult.toString().toUtf8() << "(BrowsePath: "
						   << static_cast<std::string>(browseName) << ")";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			if (uaBrowsePathResults.length() != 1) {
				LOG(ERROR) << "Expect 1 browseResult, got " << uaBrowsePathResults.length() << " for node: '"
						   << static_cast<std::string>(startNode)
						   << "' with " << uaResult.toString().toUtf8() << "(BrowsePath: "
						   << static_cast<std::string>(browseName) << ")";
				throw Exceptions::UmatiException("BrowseResult length mismatch.");
			}

			UaStatusCode uaResultElement(uaBrowsePathResults[0].StatusCode);
			if (uaResultElement.isBad()) {
				LOG(ERROR) << "Element returned bad status code: " << uaResultElement.toString().toUtf8()
						   << " for node: '"
						   << static_cast<std::string>(startNode) << "' with " << uaResult.toString().toUtf8()
						   << " (BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement);
			}

			if (uaBrowsePathResults[0].NoOfTargets != 1) {
				LOG(WARNING) << "Continuing with index 0 - expected one target, got "
							 << uaBrowsePathResults[0].NoOfTargets
							 << " for node: '" << static_cast<std::string>(startNode) << "' with "
							 << uaResult.toString().toUtf8() << "(BrowsePath: " << static_cast<std::string>(browseName)
							 << ")";
				for (int target_id = 0; target_id < uaBrowsePathResults[0].NoOfTargets; target_id++) {
					try {
						UaNodeId _targetNodeId(UaExpandedNodeId(uaBrowsePathResults[0].Targets[0].TargetId).nodeId());
						auto nodeId = Converter::UaNodeIdToModelNodeId(_targetNodeId, m_indexToUriCache).getNodeId();
						LOG(WARNING) << "Target " << target_id << " | id: " << nodeId.Uri << ";" << nodeId.Id;
					} catch (std::exception &ex) {
						LOG(ERROR) << "error  getting nodeId " << ex.what();
					}
				}
			}

			UaNodeId targetNodeId(UaExpandedNodeId(uaBrowsePathResults[0].Targets[0].TargetId).nodeId());

			return Converter::UaNodeIdToModelNodeId(targetNodeId, m_indexToUriCache).getNodeId();
		}

		std::shared_ptr<Dashboard::IDashboardDataClient::ValueSubscriptionHandle>
		OpcUaClient::Subscribe(ModelOpcUa::NodeId_t nodeId, newValueCallbackFunction_t callback) {
			return m_opcUaWrapper->SubscriptionSubscribe(nodeId, callback);
		}

		std::vector<nlohmann::json> OpcUaClient::ReadeNodeValues(std::list<ModelOpcUa::NodeId_t> modelNodeIds) {
			std::vector<nlohmann::json> ret;
			UaDataValues readValues = readValues2(modelNodeIds);

			for (uint i = 0; i < readValues.length(); ++i) {
				auto value = readValues[i];
				auto valu = Converter::UaDataValueToJsonValue(value, false);
				auto val = valu.getValue();
				ret.push_back(val);
			}
			return ret;
		}

		UaDataValues OpcUaClient::readValues2(const std::list<ModelOpcUa::NodeId_t> &modelNodeIds) {
			UaStatus uaStatus;
			UaReadValueIds readValueIds;
			readValueIds.resize(modelNodeIds.size());
			unsigned int i = 0;
			for (const auto &modelNodeId : modelNodeIds) {
				UaNodeId nodeId = Converter::ModelNodeIdToUaNodeId(modelNodeId, m_uriToIndexCache).getNodeId();
				nodeId.copyTo(&(readValueIds[i].NodeId));
				readValueIds[i].AttributeId = OpcUa_Attributes_Value;
				++i;
			}

			UaDataValues readValues;
			UaDiagnosticInfos diagnosticInfos;

			uaStatus = m_opcUaWrapper->SessionRead(
					m_defaultServiceSettings,
					0,
					OpcUa_TimestampsToReturn::OpcUa_TimestampsToReturn_Neither,
					readValueIds,
					readValues,
					diagnosticInfos);

			if (uaStatus.isNotGood()) {
				LOG(ERROR) << "Received non good status for read: " << uaStatus.toString().toUtf8();
				std::stringstream ss;
				ss << "Received non good status  for read: " << uaStatus.toString().toUtf8();
				throw Exceptions::OpcUaException(ss.str());
			}

			return readValues;
		}

		void
		OpcUaClient::createTypeMap(std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode
		>>> &bidirectionalTypeMap, const std::shared_ptr<std::map<std::string,
				std::shared_ptr<ModelOpcUa::StructureNode>>> &typeMap,
								   uint16_t namespaceIndex
		) {
			for (
				auto &typeIterator
					: *bidirectionalTypeMap) {
				if (typeIterator.second->namespaceIndex != namespaceIndex) {
					continue;
				}
// go to highest parent and then down the ladder to add / update attributes;
// create a list of pointers till parent is null
// go backwards and add / update child nodes till the end
				std::shared_ptr<std::list<std::shared_ptr<ModelOpcUa::StructureBiNode>>>
						bloodline = std::make_shared<std::list<std::shared_ptr<ModelOpcUa::StructureBiNode
				>>>();
				std::shared_ptr<ModelOpcUa::StructureBiNode> currentGeneration = typeIterator.second;
				while (nullptr != currentGeneration) {
					bloodline->
							emplace_back(currentGeneration);
					currentGeneration = currentGeneration->parent;
				}
				std::string typeName = bloodline->front()->structureNode->SpecifiedBrowseName.Uri + ";" +
									   bloodline->front()->structureNode->SpecifiedBrowseName.Name;
				ModelOpcUa::StructureNode node = bloodline->front()->structureNode.operator*();
				node.
						ofBaseDataVariableType = bloodline->front()->ofBaseDataVariableType;
				std::stringstream bloodlineStringStream;
				for (
						auto bloodlineIterator = bloodline->end();
						bloodlineIterator != bloodline->

								begin();

						) {
					--
							bloodlineIterator;
					auto ancestor = bloodlineIterator.operator*();
					bloodlineStringStream << "->" << ancestor->structureNode->SpecifiedBrowseName.Uri << ";"
										  << ancestor->structureNode->SpecifiedBrowseName.
												  Name;
					for (
							auto childIterator = ancestor->SpecifiedBiChildNodes->begin();
							childIterator != ancestor->SpecifiedBiChildNodes->

									end();

							childIterator++) {
						auto currentChild = childIterator.operator*();
						if (!currentChild->isType) {
							auto structureNode = currentChild->toStructureNode();

							auto findIterator = std::find(node.SpecifiedChildNodes->begin(),
														  node.SpecifiedChildNodes->end(), structureNode);

							bool found = findIterator != node.SpecifiedChildNodes->end();
							if (!found) {
								for (
										auto fIt = node.SpecifiedChildNodes->begin();
										fIt != node.SpecifiedChildNodes->

												end();

										fIt++) {
									if (fIt.operator*()->SpecifiedBrowseName.Name == structureNode->SpecifiedBrowseName.
											Name &&
										fIt
												.operator*()->SpecifiedBrowseName.Uri ==
										structureNode->SpecifiedBrowseName.Uri
											) {
										findIterator = fIt;
										found = true;
										break;
									}
								}
							}

							if (found) {
								if (findIterator.operator*()->ModellingRule == ModelOpcUa::ModellingRule_t::Optional ||
									findIterator.operator*()->ModellingRule ==
									ModelOpcUa::ModellingRule_t::OptionalPlaceholder) {
// LOG(INFO) << "Changed modellingRule from " << findIterator.operator*()->ModellingRule << " to " << structureNode->ModellingRule;
									node.SpecifiedChildNodes->
											erase(findIterator
														  ++);
									node.SpecifiedChildNodes->
											emplace_back(structureNode);
								}
							} else {
								node.SpecifiedChildNodes->
										emplace_back(structureNode);
							}
						}
					}
				}
				auto shared = std::make_shared<ModelOpcUa::StructureNode>(node);

				std::pair<std::string, std::shared_ptr<ModelOpcUa::StructureNode>> newType(typeName, shared);
				typeMap->
						insert(newType);
			}
		}

		void
		OpcUaClient::split(const std::string &inputString, std::vector<std::string> &resultContainer, char delimiter) {
			std::size_t current_char_position, previous_char_position = 0;
			current_char_position = inputString.find(delimiter);
			while (current_char_position != std::string::npos) {
				updateResultContainer(inputString, resultContainer, current_char_position, previous_char_position);
				previous_char_position = current_char_position + 1;
				current_char_position = inputString.find(delimiter, previous_char_position);
			}
			updateResultContainer(inputString, resultContainer, current_char_position, previous_char_position);
		}

		void
		OpcUaClient::updateResultContainer(const std::string &inputString, std::vector<std::string> &resultContainer,
										   size_t current_char_position, size_t previous_char_position) {
			std::string substr = inputString.substr(previous_char_position,
													current_char_position - previous_char_position);
			if (!substr.empty()) {
				resultContainer.push_back(substr);
			}
		}

	}
}