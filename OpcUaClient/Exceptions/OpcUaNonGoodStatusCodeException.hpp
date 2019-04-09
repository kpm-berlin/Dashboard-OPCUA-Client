#pragma once

#include <stdexcept>
#include <Exceptions/OpcUaException.hpp>
#include <statuscode.h>

namespace Umati
{
	namespace Exceptions
	{
		class OpcUaNonGoodStatusCodeException : public OpcUaException {
		public:
			using OpcUaException::OpcUaException;
			OpcUaNonGoodStatusCodeException(UaStatusCode status, std::string message);
			OpcUaNonGoodStatusCodeException(UaStatusCode status);

		protected:
			std::string statusToMessage(UaStatusCode status);
			std::string toHex(UaStatusCode status, int digits = 8);

		};
	}
}
