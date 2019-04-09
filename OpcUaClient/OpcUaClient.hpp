#pragma once

#include <IDashboardDataClient.hpp>

#include <uabase.h>
#include <uaclientsdk.h>
#include <string>
#include <memory>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

#include "Subscription.hpp"

namespace Umati {
	namespace OpcUa {
		class OpcUaClient : public UaClientSdk::UaSessionCallback, public Dashboard::IDashboardDataClient
		{
			UA_DISABLE_COPY(OpcUaClient);
		public:
			OpcUaClient(std::string serverURI);
			~OpcUaClient();

			bool disconnect();

			//Subscription Subscr;

			bool isConnected() { return m_isConnected; }

			// Inherit from IDashboardClient
			virtual std::list<BrowseResult_t> Browse(ModelOpcUa::NodeId_t startNode, ModelOpcUa::NodeId_t referenceTypeId, ModelOpcUa::NodeId_t typeDefinition) override;
			virtual ModelOpcUa::NodeId_t TranslateBrowsePathToNodeId(ModelOpcUa::NodeId_t startNode, ModelOpcUa::QualifiedName_t browseName) override;
			virtual void Subscribe(ModelOpcUa::NodeId_t nodeId, newValueCallbackFunction_t callback) override;
			virtual void UnsubscribeAll() override;

		protected:
			void connectionStatusChanged(OpcUa_UInt32 clientConnectionId, UaClientSdk::UaClient::ServerStatus serverStatus) override;

			bool connect();

			OpcUa_NodeClass readNodeClass(UaNodeId nodeId);

			void checkConnection();

			bool isSameOrSubtype(UaNodeId expectedType, UaNodeId checkType);

			// ------- Default call settings -----------
			UaClientSdk::ServiceSettings m_defaultServiceSettings;
			double m_maxAgeRead_ms = 100.0;
			OpcUa_UInt32 m_nextTransactionid = 0x80000000;
			// -----------------------------------------

			///\TODO replace by subcription to ns0;i=2255 [Server_NamespaceArray]
			void updateNamespaceCache();

			void threadConnectExecution();

			std::shared_ptr<UaClientSdk::UaSession> m_pSession;
			std::map<std::string, uint16_t> m_uriToIndexCache;
			std::map<uint16_t, std::string> m_indexToUriCache;
			std::string m_serverUri;
			std::shared_ptr<std::thread> m_connectThread;
			std::atomic_bool m_isConnected = false;
			std::atomic_bool m_tryConnecting = false;

			Subscription m_subscr;
		private:
			static int PlattformLayerInitialized;
		};
	}
}
