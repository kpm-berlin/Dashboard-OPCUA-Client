#include "OpcUaClient.hpp"
#include "ScopeExitGuard.hpp"
#include "SetupSecurity.hpp"

#include <Exceptions/ClientNotConnected.hpp>
#include "Exceptions/OpcUaNonGoodStatusCodeException.hpp"

#include "Converter/ModelNodeIdToUaNodeId.hpp"
#include "Converter/UaNodeIdToModelNodeId.hpp"
#include "Converter/ModelQualifiedNameToUaQualifiedName.hpp"
#include "Converter/UaQualifiedNameToModelQualifiedName.hpp"
#include "Converter/UaNodeClassToModelNodeClass.hpp"
#include "Converter/UaDataValueToJsonValue.hpp"

namespace Umati
{

	namespace OpcUa
	{

		
		static void stateCallback(UA_Client *client,
                          UA_SecureChannelState channelState,
                          UA_SessionState sessionState,
                          UA_StatusCode connectStatus)
		{

			switch(channelState) {
				case UA_SECURECHANNELSTATE_FRESH:
				case UA_SECURECHANNELSTATE_CLOSED:
					LOG(INFO) << "The client is disconnected";
					break;
				case UA_SECURECHANNELSTATE_HEL_SENT:
					LOG(INFO) << "Waiting for ack";
					break;
				case UA_SECURECHANNELSTATE_OPN_SENT:
					LOG(INFO) << "Waiting for OPN Response";
					break;
				case UA_SECURECHANNELSTATE_OPEN:
					LOG(INFO) << "A SecureChannel to the server is open";
					break;
				default:
					break;
				}

			switch(sessionState) {
    			case UA_SESSIONSTATE_ACTIVATED: 
      				LOG(INFO) << "A session with the server is activated";
					break;
				case UA_SESSIONSTATE_CLOSED:
					LOG(INFO) << "Session disconnected";
					break;
				default:
					break;
				}

			switch(connectStatus){
				case UA_STATUSCODE_BADDISCONNECT:
				LOG(INFO) << "Bad Disconnect";
			}
		}

		static void inactivityCallback (UA_Client *client) {
		    LOG(ERROR) << "\n\n\nINACTIVITYCALLBACK\n\n\n";
		}

		OpcUaClient::OpcUaClient(std::string serverURI, std::string Username, std::string Password,
								 std::uint8_t security, std::vector<std::string> expectedObjectTypeNamespaces,
								 std::shared_ptr<Umati::OpcUa::OpcUaInterface> opcUaWrapper)
			: m_serverUri(std::move(serverURI)), m_username(std::move(Username)), m_password(std::move(Password)),
			  m_security(static_cast<UA_MessageSecurityMode>(security)),
			 m_subscr(m_uriToIndexCache, m_indexToUriCache),
			 m_pClient(UA_Client_new(), UA_Client_delete)
			 
        {
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			UA_ClientConfig *config = UA_Client_getConfig(m_pClient.get());
			SetupSecurity::setupSecurity(config, m_pClient.get());
			config->securityMode = UA_MessageSecurityMode(security);
			UA_ApplicationDescription_clear(&config->clientDescription);
			prepareSessionConnectInfo(config->clientDescription);
			config->timeout = 2000;
			config->inactivityCallback = inactivityCallback;
			config->stateCallback = stateCallback;
			m_opcUaWrapper = std::move(opcUaWrapper);
			m_opcUaWrapper->setSubscription(&m_subscr);
			m_tryConnecting = true;
			// Try connecting at least once
			this->connect();
			m_connectThread = std::make_shared<std::thread>([this]() { this->threadConnectExecution(); });
		}

		bool OpcUaClient::connect()
		{
			open62541Cpp::UA_String sURL(m_serverUri.c_str());
			UA_StatusCode  result;

			m_opcUaWrapper->GetNewSession(m_pSession);

			if(m_username.empty() && m_password.empty()){
				result = m_opcUaWrapper->SessionConnect(m_pClient.get(), sURL);
			}else{
				result = m_opcUaWrapper->SessionConnectUsername(m_pClient.get(), sURL, m_username, m_password);
			}
			if (UA_StatusCode_isBad(result))
			{
				LOG(ERROR) << "Connecting failed in OPC UA Data Client: " << UA_StatusCode_name(result) << std::endl;
				connectionStatusChanged(0,UA_SERVERSTATE_FAILED);
				return false;	
			} 
			connectionStatusChanged(0,UA_SERVERSTATE_RUNNING);

			return true;
		}
		UA_ApplicationDescription &
		OpcUaClient::prepareSessionConnectInfo(UA_ApplicationDescription &sessionConnectInfo)
		{	
			sessionConnectInfo.applicationName = UA_LOCALIZEDTEXT_ALLOC("en-US", "KonI4.0 OPC UA Data Client");
			sessionConnectInfo.applicationUri = UA_STRING_ALLOC("http://dashboard.umati.app/OPCUA_DataClient");
		 	sessionConnectInfo.productUri = UA_STRING_ALLOC("KonI40OpcUaClient_Product");
			sessionConnectInfo.applicationType = UA_APPLICATIONTYPE_CLIENT;
			return sessionConnectInfo;
		}

		void OpcUaClient::on_connected()
		{
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			updateNamespaceCache();
			m_opcUaWrapper->SubscriptionCreateSubscription(m_pClient.get());
		}

		std::string OpcUaClient::getTypeName(const ModelOpcUa::NodeId_t &nodeId)
		{
			return readNodeBrowseName(nodeId);
		}

		std::string OpcUaClient::readNodeBrowseName(const ModelOpcUa::NodeId_t &_nodeId)
		{
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			auto nodeId = Converter::ModelNodeIdToUaNodeId(_nodeId, m_uriToIndexCache).getNodeId();
			checkConnection();

			UA_QualifiedName resultname;
			UA_QualifiedName_init(&resultname);
			auto uaResult = UA_Client_readBrowseNameAttribute(m_pClient.get(), *nodeId.NodeId, &resultname);

			if (UA_StatusCode_isBad(uaResult))
			{
				LOG(ERROR) << "readNodeClass failed for node: '" << nodeId.NodeId->identifier.string.data
						   << "' with " << UA_StatusCode_name(uaResult);
				UA_QualifiedName_clear(&resultname);
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}
			std::string resName = std::string((char *)resultname.name.data,resultname.name.length);

			UA_QualifiedName_clear(&resultname);

			return _nodeId.Uri + ";" + resName;
		}

		UA_NodeClass OpcUaClient::readNodeClass(const open62541Cpp::UA_NodeId &nodeId)
		{
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			checkConnection();

			UA_NodeClass returnClass;
			UA_NodeClass_init(&returnClass);
			try{
			auto uaResult = UA_Client_readNodeClassAttribute(m_pClient.get(), *nodeId.NodeId, &returnClass);
			if (UA_StatusCode_isBad(uaResult))
			{
				LOG(ERROR) << "readNodeClass failed";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}
			}catch(...){
				LOG(ERROR) << "readNodeClass failed";
			}
			return returnClass;
		}

		void OpcUaClient::checkConnection()
		{
			if (!this->m_isConnected || !m_opcUaWrapper->SessionIsConnected(m_pClient.get()))
			{
				connectionStatusChanged(0,UA_SERVERSTATE_FAILED);
				throw Exceptions::ClientNotConnected("Need connected client.");
			}
		}

		open62541Cpp::UA_NodeId OpcUaClient::browseSuperType(const open62541Cpp::UA_NodeId &typeNodeId)
		{
			open62541Cpp::UA_NodeId referenceTypeUaNodeId;
			referenceTypeUaNodeId.NodeId->identifierType = UA_NODEIDTYPE_NUMERIC;
			referenceTypeUaNodeId.NodeId->identifier.numeric = UA_NS0ID_HASSUBTYPE;

			UA_BrowseDescription browseContext;
			browseContext.browseDirection = UA_BROWSEDIRECTION_INVERSE;
			browseContext.includeSubtypes = UA_TRUE;

			browseContext.nodeClassMask = 0; // ALL
			UA_NodeId_copy(referenceTypeUaNodeId.NodeId,&browseContext.referenceTypeId);
			browseContext.resultMask = UA_BROWSERESULTMASK_NONE;

			UA_NodeClass nodeClass = readNodeClass(typeNodeId);
			
			switch (nodeClass)
			{
			case UA_NODECLASS_OBJECTTYPE:
			case UA_NODECLASS_VARIABLETYPE:
			{
				browseContext.nodeClassMask = nodeClass;
				break;
			}
			default:
				LOG(ERROR) << "Invalid NodeClass " << nodeClass;
				throw Exceptions::UmatiException("Invalid NodeClass");
			}


            std::lock_guard<std::recursive_mutex> l(m_clientMutex);

			UA_ByteString continuationPoint;
			std::vector<UA_ReferenceDescription> referenceDescriptions;
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_pClient.get(), /*m_defaultServiceSettings,*/ typeNodeId, browseContext,
														  continuationPoint, referenceDescriptions);
			
			if (UA_StatusCode_isBad(uaResult.results->statusCode))
			{
				LOG(ERROR) << "Bad return from browse: " << uaResult.results->statusCode;
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult.results->statusCode);
			}

			UA_BrowseResponse_clear(&uaResult);
			std::list<ModelOpcUa::BrowseResult_t> browseResult;
			if (referenceDescriptions.size() == 0)
			{
				return open62541Cpp::UA_NodeId();
			}
			if (referenceDescriptions.size() > 1)
			{
				LOG(ERROR) << "Found multiple superTypes for " << typeNodeId.NodeId->identifier.string.data;
				return open62541Cpp::UA_NodeId();
			}
		
			UA_ExpandedNodeId retExpandedNodeId;
			UA_ExpandedNodeId_copy(&referenceDescriptions.at(0).nodeId,&retExpandedNodeId);

			return open62541Cpp::UA_NodeId(retExpandedNodeId.nodeId);
		}

		bool
		OpcUaClient::isSameOrSubtype(const open62541Cpp::UA_NodeId &expectedType, const open62541Cpp::UA_NodeId &checkType, std::size_t maxDepth)
		{

			UA_NodeId checkNode = *checkType.NodeId;
			UA_NodeId expectedNode = *expectedType.NodeId;

			if (UA_NodeId_isNull(&checkNode))
			{
				return false;
			}

			if (UA_NodeId_equal(&expectedNode, &checkNode))
			{
				return true;
			}

			auto it = m_superTypes.find(checkType);

			if (it != m_superTypes.end())
			{
				return isSameOrSubtype(expectedType, it->second, --maxDepth);
			}

			auto superType = browseSuperType(checkType);
			m_superTypes[checkType] = superType;
			return isSameOrSubtype(expectedType, superType, --maxDepth);
		}

		void OpcUaClient::initializeNamespaceCache()
		{
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			m_opcUaWrapper->SessionUpdateNamespaceTable(m_pClient.get());

			m_uriToIndexCache.clear();
			m_indexToUriCache.clear();
		}

		void OpcUaClient::updateNamespaceCache()
		{
			initializeNamespaceCache();

	    	auto uaNamespaces = m_opcUaWrapper->SessionGetNamespaceTable();
		
			fillNamespaceCache(uaNamespaces);
		}

		void OpcUaClient::fillNamespaceCache(const std::vector<std::string> &uaNamespaces)
		{
			for(size_t i = 0; i < uaNamespaces.size(); ++i){

				std::string namespaceURI = uaNamespaces.at(i);
				
				if ( m_uriToIndexCache.find(namespaceURI) == m_uriToIndexCache.end() ) {
				    m_uriToIndexCache[namespaceURI] = static_cast<uint16_t>(i);
					m_indexToUriCache[static_cast<uint16_t>(i)] = namespaceURI;
				} else {
                    LOG(INFO) << "Namespace already in cache";
				}
		
				LOG(INFO) << "index: " << std::to_string(i) << ", namespaceURI: " << namespaceURI;
			} 
			
		}

		ModelOpcUa::ModellingRule_t OpcUaClient::browseModellingRule(const open62541Cpp::UA_NodeId &uaNodeId)
		{
			UA_ByteString continuationPoint;
			std::vector<UA_ReferenceDescription> referenceDescriptions;

			/// begin browse modelling rule
			UA_BrowseDescription browseContext2 = getUaBrowseContext(prepareObjectAndVariableTypeBrowseContext());
			browseContext2.referenceTypeId.identifier.numeric= UA_NS0ID_HASMODELLINGRULE;
			ModelOpcUa::ModellingRule_t modellingRule = ModelOpcUa::ModellingRule_t::Optional;

            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			auto uaResult2 = m_opcUaWrapper->SessionBrowse(m_pClient.get(), /*m_defaultServiceSettings,*/ uaNodeId,
														   browseContext2,
														   continuationPoint, referenceDescriptions);
			if (UA_StatusCode_isBad(uaResult2.results->statusCode))
			{
				LOG(ERROR) << "Bad return from browse: " << uaResult2.results->statusCode << "for nodeId"
						   << uaNodeId.NodeId->identifier.string.data;
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult2.results->statusCode);
			}

			UA_BrowseDescription_clear(&browseContext2);
			UA_BrowseResponse_clear(&uaResult2);
			
			for (UA_Int32 i = 0; i < referenceDescriptions.size(); i++)
			{
				auto refDescr = referenceDescriptions.at(i);
				ModelOpcUa::BrowseResult_t browseResult = ReferenceDescriptionToBrowseResult(refDescr);
				if (browseResult.BrowseName.Name == "Mandatory")
				{
					modellingRule = ModelOpcUa::Mandatory;
				}
				else if (browseResult.BrowseName.Name == "Optional")
				{
					modellingRule = ModelOpcUa::Optional;
				}
				else if (browseResult.BrowseName.Name == "MandatoryPlaceholder")
				{
					modellingRule = ModelOpcUa::MandatoryPlaceholder;
				}
				else if (browseResult.BrowseName.Name == "OptionalPlaceholder")
				{
					modellingRule = ModelOpcUa::OptionalPlaceholder;
				}
			}
			return modellingRule;
		}

		Umati::Dashboard::IDashboardDataClient::BrowseContext_t OpcUaClient::prepareObjectAndVariableTypeBrowseContext()
		{
			Umati::Dashboard::IDashboardDataClient::BrowseContext_t ret;
			ret.nodeClassMask =
				(std::uint32_t)Umati::Dashboard::IDashboardDataClient::BrowseContext_t::NodeClassMask::OBJECT |
				(std::uint32_t)Umati::Dashboard::IDashboardDataClient::BrowseContext_t::NodeClassMask::OBJECT_TYPE |
				(std::uint32_t)Umati::Dashboard::IDashboardDataClient::BrowseContext_t::NodeClassMask::VARIABLE |
				(std::uint32_t)Umati::Dashboard::IDashboardDataClient::BrowseContext_t::NodeClassMask::VARIABLE_TYPE;
			return ret;
		}

		void OpcUaClient::connectionStatusChanged(UA_Int32  /*clientConnectionId*/,UA_ServerState serverStatus) 
		{
			switch (serverStatus)
			{
			case UA_SERVERSTATE_FAILED:
				LOG(ERROR) << "Disconnected." << std::endl;
				m_isConnected = false;
				break;
			case UA_SERVERSTATE_RUNNING:
				LOG(ERROR) << "Connected." << std::endl;
				m_isConnected = true;
				on_connected();
				break;
			case UA_SERVERSTATE_SHUTDOWN:
				LOG(ERROR) << "ServerShutdown." << std::endl;
				break;
			case UA_SESSIONSTATE_CREATED:
				LOG(ERROR) << "NewSessionCreated." << std::endl;
				break;
			
			}
		}
		

		OpcUaClient::~OpcUaClient()
		{
			m_tryConnecting = false;
			if (m_connectThread)
			{
				m_connectThread->join();
			}

			m_subscr.deleteSubscription(m_pClient.get());
			disconnect();
		}

		bool OpcUaClient::disconnect()
		{
						//VERIFY do we need this session?
			if (m_pSession)
			{
				m_subscr.deleteSubscription(m_pClient.get());
				return (m_opcUaWrapper->SessionDisconnect(m_pClient.get(), UA_TRUE) != UA_STATUSCODE_GOOD) ? false : true;
			}
			return true;
		}

		void OpcUaClient::threadConnectExecution()
		{
			while (m_tryConnecting)
			{
				if (!m_isConnected)
				{
					this->connect();
				}
				else
				{
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
		}
		std::list<ModelOpcUa::BrowseResult_t> OpcUaClient::Browse(
			ModelOpcUa::NodeId_t startNode,
			BrowseContext_t browseContext)
		{
			UA_BrowseDescription uaBrowseContext = getUaBrowseContext(browseContext);
			return BrowseWithContextAndFilter(startNode, uaBrowseContext);
		}

		std::list<ModelOpcUa::BrowseResult_t> OpcUaClient::BrowseWithResultTypeFilter(
			ModelOpcUa::NodeId_t startNode,
			BrowseContext_t browseContext,
			ModelOpcUa::NodeId_t typeDefinition)
		{
			UA_BrowseDescription uaBrowseContext = getUaBrowseContext(browseContext);
			open62541Cpp::UA_NodeId typeDefinitionUaNodeId = Converter::ModelNodeIdToUaNodeId(
											  typeDefinition,
											  m_uriToIndexCache)
											  .getNodeId();

			uaBrowseContext.nodeClassMask = nodeClassFromNodeId(typeDefinitionUaNodeId);
			auto filter = [&](const UA_ReferenceDescription &ref) {
				open62541Cpp::UA_NodeId browseTypeNodeId = open62541Cpp::UA_NodeId(ref.typeDefinition.nodeId);
				return isSameOrSubtype(typeDefinitionUaNodeId, browseTypeNodeId);
			};
			return BrowseWithContextAndFilter(startNode, uaBrowseContext, filter);
		}

		UA_NodeClass OpcUaClient::nodeClassFromNodeId(const open62541Cpp::UA_NodeId &typeDefinitionUaNodeId)
		{
			UA_NodeClass nodeClass = readNodeClass(typeDefinitionUaNodeId);
			
			switch (nodeClass)
			{
			case UA_NODECLASS_OBJECTTYPE:
			{	
				return UA_NODECLASS_OBJECT;
				break;
			}
			case UA_NODECLASS_VARIABLETYPE:
			{
				return UA_NODECLASS_VARIABLE;
				break;
			}
			default:
				LOG(ERROR) << "Invalid NodeClass " << nodeClass
						   << " expect object or variable type for node "
						   << typeDefinitionUaNodeId.NodeId->identifier.string.data;
				throw Exceptions::UmatiException("Invalid NodeClass");
			}
		}

		std::list<ModelOpcUa::BrowseResult_t> OpcUaClient::BrowseWithContextAndFilter(
			const ModelOpcUa::NodeId_t &startNode,
			UA_BrowseDescription &browseContext,
			std::function<bool(const UA_ReferenceDescription &)> filter)
		{
			Converter::ModelNodeIdToUaNodeId conv = Converter::ModelNodeIdToUaNodeId(startNode, m_uriToIndexCache);
			open62541Cpp::UA_NodeId startUaNodeId = conv.getNodeId();

			UA_ByteString continuationPoint;
			UA_ByteString_init(&continuationPoint);
			std::vector<UA_ReferenceDescription> referenceDescriptions;

            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			checkConnection();
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_pClient.get(), /*m_defaultServiceSettings,*/ startUaNodeId, browseContext,
														  continuationPoint, referenceDescriptions);

			ScopeExitGuard browseGuard([&]() {
			UA_BrowseDescription_clear(&browseContext);
			UA_BrowseResponse_clear(&uaResult);
			UA_ByteString_clear(&continuationPoint);
			});

			if (uaResult.resultsSize > 0 && UA_StatusCode_isBad(uaResult.results->statusCode))
			{
				LOG(ERROR) << "Bad return from browse: " << uaResult.results->references->browseName.name.data << ", with startUaNodeId "
						   << startUaNodeId.NodeId->identifier.string.data
						   << " and ref id " << browseContext.referenceTypeId.identifier.string.data;
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult.results->statusCode);
			}

			std::list<ModelOpcUa::BrowseResult_t> browseResult;
			ReferenceDescriptionsToBrowseResults(referenceDescriptions, browseResult, filter);

			handleContinuationPoint(continuationPoint);

			return browseResult;
		}

		void OpcUaClient::ReferenceDescriptionsToBrowseResults(
			const std::vector<UA_ReferenceDescription> &referenceDescriptions,
			std::list<ModelOpcUa::BrowseResult_t> &browseResult,
			std::function<bool(const UA_ReferenceDescription &)> filter)
		{
				for (UA_Int32 i = 0; i < referenceDescriptions.size(); i++)
			{
				if (!filter(referenceDescriptions.at(i)))
				{					
				continue;
				}
					UA_ReferenceDescription tmp = referenceDescriptions.at(i);
					auto entry = ReferenceDescriptionToBrowseResult(tmp);
					browseResult.push_back(entry);
			}
		}

		void OpcUaClient::handleContinuationPoint(const UA_ByteString & /*continuationPoint*/)
		{
			// LOG(DEBUG) << "Handling continuation point not yet implemented";
		}

		ModelOpcUa::BrowseResult_t
		OpcUaClient::ReferenceDescriptionToBrowseResult(const UA_ReferenceDescription &referenceDescription)
		{
			ModelOpcUa::BrowseResult_t entry;

			open62541Cpp::UA_NodeId browseTypeUaNodeId;
			UA_NodeId_copy(&referenceDescription.typeDefinition.nodeId, browseTypeUaNodeId.NodeId);

			entry.NodeClass = Converter::UaNodeClassToModelNodeClass(referenceDescription.nodeClass).getNodeClass();
			entry.TypeDefinition = Converter::UaNodeIdToModelNodeId(browseTypeUaNodeId,m_indexToUriCache).getNodeId();

			open62541Cpp::UA_NodeId referenceDescriptionTmp;
			UA_NodeId_copy(&referenceDescription.nodeId.nodeId, referenceDescriptionTmp.NodeId);
			entry.NodeId = Converter::UaNodeIdToModelNodeId(referenceDescriptionTmp, m_indexToUriCache).getNodeId();

			open62541Cpp::UA_NodeId referenceTypeUaNodeId;
			UA_NodeId_copy(&referenceDescription.referenceTypeId, referenceTypeUaNodeId.NodeId);

			auto referenceTypeModelNodeId = Converter::UaNodeIdToModelNodeId(referenceTypeUaNodeId,m_indexToUriCache).getNodeId();
		
			entry.ReferenceTypeId = referenceTypeModelNodeId;
			open62541Cpp::UA_QualifiedName browseName(referenceDescription.browseName.namespaceIndex,
														std::string((char*)referenceDescription.browseName.name.data,
														referenceDescription.browseName.name.length));

			entry.BrowseName = Converter::UaQualifiedNameToModelQualifiedName(browseName,m_indexToUriCache).getQualifiedName();

			return entry;		
		}

		UA_BrowseDescription OpcUaClient::prepareBrowseContext(ModelOpcUa::NodeId_t referenceTypeId)
		{
			auto referenceTypeUaNodeId = Converter::ModelNodeIdToUaNodeId(std::move(referenceTypeId),
																		  m_uriToIndexCache)
											 .getNodeId();
			UA_BrowseDescription browseContext;
            UA_BrowseDescription_init(&browseContext);
			browseContext.browseDirection = UA_BROWSEDIRECTION_FORWARD;
			browseContext.includeSubtypes = UA_TRUE;
			browseContext.nodeClassMask = 0; // ALL
			if (UA_NodeId_isNull(referenceTypeUaNodeId.NodeId))
			{
				UA_NodeId_copy(referenceTypeUaNodeId.NodeId, &browseContext.referenceTypeId);
			}
			browseContext.resultMask =
				UA_BROWSERESULTMASK_BROWSENAME + UA_BROWSERESULTMASK_TYPEDEFINITION + UA_BROWSERESULTMASK_NODECLASS + UA_BROWSERESULTMASK_REFERENCETYPEID;
			return browseContext;
		}
		UA_BrowseDescription OpcUaClient::getUaBrowseContext(const IDashboardDataClient::BrowseContext_t &browseContext)
		{
			UA_BrowseDescription ret;
            UA_BrowseDescription_init(&ret);
			ret.browseDirection = (decltype(ret.browseDirection))browseContext.browseDirection;
			ret.includeSubtypes = browseContext.includeSubtypes ?  UA_TRUE : UA_FALSE;
			ret.nodeClassMask = (decltype(ret.nodeClassMask))browseContext.nodeClassMask;

			if(!browseContext.referenceTypeId.isNull())
			{
				ret.referenceTypeId = *Converter::ModelNodeIdToUaNodeId(browseContext.referenceTypeId, m_uriToIndexCache).getNodeId().NodeId;
			}else{
				UA_NodeId_clear(&ret.referenceTypeId);
			}
			ret.resultMask = (decltype(ret.resultMask))browseContext.resultMask;
			return ret;
		}

		ModelOpcUa::NodeId_t
		OpcUaClient::TranslateBrowsePathToNodeId(ModelOpcUa::NodeId_t startNode,
												 ModelOpcUa::QualifiedName_t browseName)
		{
			checkConnection();

			if (browseName.isNull())
			{
				LOG(ERROR) << "browseName is NULL";
				throw std::invalid_argument("browseName is NULL");
			}

			if (startNode.isNull())
			{
				LOG(ERROR) << "startNode is NULL";
				throw std::invalid_argument("startNode is NULL");
			}

			auto startUaNodeId = Converter::ModelNodeIdToUaNodeId(startNode,
																  m_uriToIndexCache)
																  .getNodeId();

			auto uaBrowseName = Converter::ModelQualifiedNameToUaQualifiedName(browseName,
																			   m_uriToIndexCache)
																			   .detach();

			UA_BrowsePath uaBrowsePaths;
			UA_BrowsePath_init(&uaBrowsePaths);
			uaBrowsePaths.relativePath.elementsSize = 1;
			uaBrowsePaths.relativePath.elements = UA_RelativePathElement_new();
            uaBrowsePaths.relativePath.elements->includeSubtypes = UA_TRUE;
			uaBrowsePaths.relativePath.elements->isInverse = UA_FALSE;
			uaBrowsePaths.relativePath.elements->referenceTypeId.identifier.numeric = UA_NS0ID_HIERARCHICALREFERENCES;
			uaBrowsePaths.relativePath.elements->targetName = uaBrowseName;
			UA_NodeId_copy(startUaNodeId.NodeId, &uaBrowsePaths.startingNode);

			UA_BrowsePathResult uaBrowsePathResults;
			UA_DiagnosticInfo uaDiagnosticInfos;

			ScopeExitGuard browseGuard([&]() {
				UA_BrowsePathResult_clear(&uaBrowsePathResults);
				UA_BrowsePath_clear(&uaBrowsePaths);
			});
            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			auto uaResult = m_opcUaWrapper->SessionTranslateBrowsePathsToNodeIds(
				m_pClient.get(),
				uaBrowsePaths,
				uaBrowsePathResults,
				uaDiagnosticInfos);

            if (UA_StatusCode_isBad(uaResult))
			{
                if (uaResult != UA_STATUSCODE_BADNOMATCH) {
                    LOG(ERROR) << "TranslateBrowsePathToNodeId failed for node: '" << static_cast<std::string>(startNode)
						   << "' with " << UA_StatusCode_name(uaResult) << "(BrowsePath: "
						   << static_cast<std::string>(browseName) << ")";
                }
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}
			if (uaBrowsePathResults.targetsSize != 1)
			{
				LOG(ERROR) << "Expect 1 browseResult, got " << uaBrowsePathResults.targetsSize << " for node: '"
						   << static_cast<std::string>(startNode)
						   << "' with " << uaResult << "(BrowsePath: "
						   << static_cast<std::string>(browseName) << ")";
				throw Exceptions::UmatiException("BrowseResult length mismatch.");
			}

			UA_StatusCode uaResultElement(uaBrowsePathResults.statusCode);
			
			if (UA_StatusCode_isBad(uaResultElement))
			{
				std::stringstream ss;
				ss << "Element returned bad status code: " << uaResultElement
						   << " for node: '"
						   << static_cast<std::string>(startNode) << "' with " << uaResult
						   << " (BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement, ss.str());
			}

			if (uaBrowsePathResults.targetsSize != 1)
			{
				LOG(WARNING) << "Continuing with index 0 - expected one target, got "
							 << uaBrowsePathResults.targetsSize
							 << " for node: '" << static_cast<std::string>(startNode) << "' with "
							 << uaResult << "(BrowsePath: " << static_cast<std::string>(browseName)
							 << ")";
				for (int target_id = 0; target_id < uaBrowsePathResults.targetsSize; target_id++)
				{
					try
					{
						open62541Cpp::UA_NodeId _targetNodeId(uaBrowsePathResults.targets->targetId.nodeId);
						auto nodeId = Converter::UaNodeIdToModelNodeId(_targetNodeId, m_indexToUriCache).getNodeId();
						LOG(WARNING) << "Target " << target_id << " | id: " << nodeId.Uri << ";" << nodeId.Id;
					}
					catch (std::exception &ex)
					{
						LOG(ERROR) << "error  getting nodeId " << ex.what();
					}
				}
			}

			open62541Cpp::UA_NodeId targetNodeId(uaBrowsePathResults.targets->targetId.nodeId);

			return Converter::UaNodeIdToModelNodeId(targetNodeId, m_indexToUriCache).getNodeId();
			}

		std::shared_ptr<Dashboard::IDashboardDataClient::ValueSubscriptionHandle>
		OpcUaClient::Subscribe(ModelOpcUa::NodeId_t nodeId, newValueCallbackFunction_t callback)
		{
			return m_opcUaWrapper->SubscriptionSubscribe(m_pClient.get(), nodeId, callback);
		}
	
		std::vector<nlohmann::json> OpcUaClient::ReadeNodeValues(std::list<ModelOpcUa::NodeId_t> modelNodeIds)
		{
			return readValues2(modelNodeIds);
		}

		std::vector<nlohmann::json> OpcUaClient::readValues2(const std::list<ModelOpcUa::NodeId_t> &modelNodeIds)
		{

			std::vector<nlohmann::json> readValues;

            std::lock_guard<std::recursive_mutex> l(m_clientMutex);
			for (const auto &modelNodeId : modelNodeIds)
			{
				/// \todo use single read request for all values
				open62541Cpp::UA_NodeId nodeId = Converter::ModelNodeIdToUaNodeId(modelNodeId, m_uriToIndexCache).getNodeId();

				UA_DataValue tmpReadValue;
				UA_DataValue_init(&tmpReadValue);
			
				UA_ReadValueId readValueId;
				UA_ReadValueId_init(&readValueId);

				readValueId.attributeId = UA_ATTRIBUTEID_VALUE;
				readValueId.nodeId = *nodeId.NodeId;

				UA_DiagnosticInfo info;
				UA_DiagnosticInfo_init(&info);

				auto ret = m_opcUaWrapper->SessionRead(m_pClient.get(),0.0,UA_TIMESTAMPSTORETURN_BOTH,readValueId, tmpReadValue, info);

				if (UA_StatusCode_isBad(ret))
				{
					LOG(ERROR) << "Received non good status for reading node(" << static_cast<std::string>(modelNodeId) << "): " << UA_StatusCode_name(ret);
					std::stringstream ss;
					ss << "Received non good status  for read: " << ret;
					UA_DataValue_clear(&tmpReadValue);
					throw Exceptions::OpcUaException(ss.str());
				}
				else
				{
					auto valu = Converter::UaDataValueToJsonValue(tmpReadValue, false);
					auto val = valu.getValue();
					readValues.push_back(val);
					UA_DataValue_clear(&tmpReadValue);
				}
			}
			return readValues;
		}

		std::vector<std::string> OpcUaClient::Namespaces()
		{
			std::vector<std::string> ret;
			for(auto &ns : m_indexToUriCache)
			{
				ret.push_back(ns.second);
			}
			return ret;
		}

	} // namespace OpcUa
} // namespace Umati
