#include "can-lite/core/CanCategoryOutbound.hpp"

namespace services
{
    CanCategoryOutboundNull& CanCategoryOutboundNull::Instance()
    {
        static CanCategoryOutboundNull instance;
        return instance;
    }

    uint8_t CanCategoryOutboundNull::Category() const
    {
        return 0;
    }

    uint16_t CanCategoryOutboundNull::NodeId() const
    {
        return 0;
    }

    CanRequestContext CanCategoryOutboundNull::CurrentRequest() const
    {
        return CanRequestContext{};
    }

    bool CanCategoryOutboundNull::Send(CanPriority, uint8_t, const hal::Can::Message&)
    {
        return false;
    }

    bool CanCategoryOutboundNull::SendTo(uint16_t, CanPriority, uint8_t, const hal::Can::Message&)
    {
        return false;
    }

    bool CanCategoryOutboundNull::SendSequencedTo(uint16_t, CanPriority, uint8_t, const hal::Can::Message&)
    {
        return false;
    }

    void CanCategoryOutboundNull::SendAck(uint8_t, CanAckStatus)
    {}

    void CanCategoryOutboundNull::SendAckFor(const CanRequestContext&, uint8_t, CanAckStatus)
    {}

    void CanCategoryOutboundImpl::Bind(CanFrameTransport& newTransport, uint8_t newCategoryId)
    {
        transport = &newTransport;
        categoryId = newCategoryId;
        currentRequest = CanRequestContext{};
        sequences.Forget();
    }

    void CanCategoryOutboundImpl::Unbind()
    {
        transport = nullptr;
        categoryId = 0;
        currentRequest = CanRequestContext{};
        sequences.Forget();
    }

    bool CanCategoryOutboundImpl::IsBound() const
    {
        return transport != nullptr;
    }

    bool CanCategoryOutboundImpl::IsBoundTo(uint8_t queriedCategoryId) const
    {
        return transport != nullptr && categoryId == queriedCategoryId;
    }

    void CanCategoryOutboundImpl::BeginRequest(uint16_t requestPeerNodeId, uint8_t requestCorrelation)
    {
        currentRequest = CanRequestContext{ requestPeerNodeId, requestCorrelation };
    }

    CanSequenceTable::ValidationResult CanCategoryOutboundImpl::ValidateSequence(uint16_t peer, uint8_t sequenceNumber)
    {
        return sequences.Validate(peer, sequenceNumber);
    }

    void CanCategoryOutboundImpl::ResyncSequence(uint16_t peer, uint8_t nextSequenceNumber)
    {
        sequences.Resync(peer, nextSequenceNumber);
    }

    void CanCategoryOutboundImpl::SendAckWith(uint16_t targetNodeId, uint8_t messageType, CanAckStatus status,
        uint8_t ackCorrelation, uint8_t expectedSequence)
    {
        if (transport == nullptr)
            return;

        hal::Can::Message msg;
        msg.push_back(categoryId);
        msg.push_back(messageType);
        msg.push_back(static_cast<uint8_t>(status));
        msg.push_back(ackCorrelation);
        msg.push_back(expectedSequence);

        transport->SendFrame(targetNodeId, CanPriority::response, canSystemCategoryId,
            canCommandAckMessageTypeId, msg, [] {});
    }

    uint8_t CanCategoryOutboundImpl::Category() const
    {
        return categoryId;
    }

    uint16_t CanCategoryOutboundImpl::NodeId() const
    {
        return transport != nullptr ? transport->NodeId() : uint16_t{ 0 };
    }

    CanRequestContext CanCategoryOutboundImpl::CurrentRequest() const
    {
        return currentRequest;
    }

    bool CanCategoryOutboundImpl::Send(CanPriority priority, uint8_t messageType, const hal::Can::Message& payload)
    {
        if (transport == nullptr)
            return false;

        return transport->SendFrame(priority, categoryId, messageType, payload, [] {});
    }

    bool CanCategoryOutboundImpl::SendTo(uint16_t targetNodeId, CanPriority priority, uint8_t messageType,
        const hal::Can::Message& payload)
    {
        if (transport == nullptr)
            return false;

        return transport->SendFrame(targetNodeId, priority, categoryId, messageType, payload, [] {});
    }

    bool CanCategoryOutboundImpl::SendSequencedTo(uint16_t targetNodeId, CanPriority priority, uint8_t messageType,
        const hal::Can::Message& payload)
    {
        if (transport == nullptr)
            return false;

        // A broadcast carries no correlatable resynchronisation path: every
        // server would validate the same peer key and answer from its own
        // address, so a sequenceError could never be attributed to the stream
        // that drifted. Refuse before a sequence number is consumed.
        if (targetNodeId == canBroadcastNodeId)
            return false;

        if (payload.size() >= payload.max_size())
            return false;

        hal::Can::Message sequenced;
        sequenced.push_back(sequences.Allocate(targetNodeId));
        for (auto byte : payload)
            sequenced.push_back(byte);

        return transport->SendFrame(targetNodeId, priority, categoryId, messageType, sequenced, [] {});
    }

    void CanCategoryOutboundImpl::SendAck(uint8_t messageType, CanAckStatus status)
    {
        SendAckFor(currentRequest, messageType, status);
    }

    void CanCategoryOutboundImpl::SendAckFor(const CanRequestContext& request, uint8_t messageType, CanAckStatus status)
    {
        SendAckWith(NodeId(), messageType, status, request.correlation, 0);
    }
}
