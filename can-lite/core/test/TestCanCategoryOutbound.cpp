#include "can-lite/core/CanCategoryOutbound.hpp"
#include "can-lite/core/CanFrameTransport.hpp"
#include "can-lite/core/CanProtocolDefinitions.hpp"
#include "can-lite/core/test/CanMock.hpp"
#include "infra/util/BoundedVector.hpp"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>

namespace
{
    using namespace testing;
    using namespace services;

    constexpr uint16_t ownNodeId = 7;
    constexpr uint16_t peerNodeId = 5;
    constexpr uint16_t otherPeerNodeId = 6;
    constexpr uint8_t boundCategoryId = 0x03;
    constexpr uint8_t messageTypeId = 0x21;

    class CanCategoryOutboundTest
        : public ::testing::Test
    {
    public:
        void RecordSentFrames()
        {
            EXPECT_CALL(canMock, SendData(_, _, _)).Times(AnyNumber()).WillRepeatedly(Invoke([this](hal::Can::Id id, const hal::Can::Message& data, const infra::Function<void(bool)>& onCompletion)
                {
                    sentIds.push_back(id.Get29BitId());
                    sentFrames.push_back(data);
                    onCompletion(true);
                }));
        }

        hal::Can::Message MakePayload(std::size_t size) const
        {
            hal::Can::Message payload;
            while (payload.size() != size)
                payload.push_back(static_cast<uint8_t>(0xB0 + payload.size()));
            return payload;
        }

        std::size_t MaxFrameSize() const
        {
            return hal::Can::Message{}.max_size();
        }

        bool SendSequenced(uint16_t targetNodeId, std::size_t payloadSize = 1)
        {
            return outbound.SendSequencedTo(targetNodeId, CanPriority::command, messageTypeId, MakePayload(payloadSize));
        }

        StrictMock<hal::CanMock> canMock;
        CanFrameTransport transport{ canMock, ownNodeId };
        CanCategoryOutboundImpl outbound;
        infra::BoundedVector<uint32_t>::WithMaxSize<8> sentIds;
        infra::BoundedVector<hal::Can::Message>::WithMaxSize<8> sentFrames;
    };

    // === CanCategoryOutboundNull ===

    TEST_F(CanCategoryOutboundTest, Null_ReportsNeutralIdentity)
    {
        auto& null = CanCategoryOutboundNull::Instance();

        EXPECT_EQ(null.Category(), 0);
        EXPECT_EQ(null.NodeId(), 0);
        EXPECT_EQ(null.CurrentRequest().peerNodeId, 0);
        EXPECT_EQ(null.CurrentRequest().correlation, 0);
    }

    TEST_F(CanCategoryOutboundTest, Null_RefusesEverySendWithoutEmittingAFrame)
    {
        auto& null = CanCategoryOutboundNull::Instance();
        auto payload = MakePayload(2);

        EXPECT_FALSE(null.Send(CanPriority::response, messageTypeId, payload));
        EXPECT_FALSE(null.SendTo(peerNodeId, CanPriority::command, messageTypeId, payload));
        EXPECT_FALSE(null.SendSequencedTo(peerNodeId, CanPriority::command, messageTypeId, payload));
    }

    TEST_F(CanCategoryOutboundTest, Null_SendAckEmitsNothing)
    {
        CanCategoryOutboundNull::Instance().SendAck(messageTypeId, CanAckStatus::success);
        CanCategoryOutboundNull::Instance().SendAckFor(CanRequestContext{ peerNodeId, 3 }, messageTypeId, CanAckStatus::success);
    }

    // === Bind / Unbind ===

    TEST_F(CanCategoryOutboundTest, Unbound_IsNotBoundToAnyCategory)
    {
        EXPECT_FALSE(outbound.IsBound());
        EXPECT_FALSE(outbound.IsBoundTo(boundCategoryId));
    }

    TEST_F(CanCategoryOutboundTest, Bind_BindsToTheGivenCategoryOnly)
    {
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(outbound.IsBound());
        EXPECT_TRUE(outbound.IsBoundTo(boundCategoryId));
        EXPECT_FALSE(outbound.IsBoundTo(boundCategoryId + 1));
        EXPECT_EQ(outbound.Category(), boundCategoryId);
        EXPECT_EQ(outbound.NodeId(), ownNodeId);
    }

    TEST_F(CanCategoryOutboundTest, Unbind_LeavesTheHandleUnbound)
    {
        outbound.Bind(transport, boundCategoryId);
        outbound.BeginRequest(peerNodeId, 0x42);
        outbound.Unbind();

        EXPECT_FALSE(outbound.IsBound());
        EXPECT_FALSE(outbound.IsBoundTo(boundCategoryId));
        EXPECT_EQ(outbound.NodeId(), 0);
        EXPECT_EQ(outbound.CurrentRequest().peerNodeId, 0);
        EXPECT_EQ(outbound.CurrentRequest().correlation, 0);
    }

    // === Sending while unbound ===

    TEST_F(CanCategoryOutboundTest, Unbound_RefusesEverySendWithoutEmittingAFrame)
    {
        auto payload = MakePayload(2);

        EXPECT_FALSE(outbound.Send(CanPriority::response, messageTypeId, payload));
        EXPECT_FALSE(outbound.SendTo(peerNodeId, CanPriority::command, messageTypeId, payload));
        EXPECT_FALSE(outbound.SendSequencedTo(peerNodeId, CanPriority::command, messageTypeId, payload));
    }

    TEST_F(CanCategoryOutboundTest, Unbound_SendAckEmitsNothing)
    {
        outbound.SendAckWith(peerNodeId, messageTypeId, CanAckStatus::invalidPayload, 3, 4);
        outbound.SendAck(messageTypeId, CanAckStatus::invalidPayload);
        outbound.SendAckFor(CanRequestContext{ peerNodeId, 3 }, messageTypeId, CanAckStatus::invalidPayload);
    }

    TEST_F(CanCategoryOutboundTest, Unbound_AfterUnbind_RefusesEverySendWithoutEmittingAFrame)
    {
        outbound.Bind(transport, boundCategoryId);
        outbound.Unbind();

        EXPECT_FALSE(outbound.Send(CanPriority::response, messageTypeId, MakePayload(2)));
        EXPECT_FALSE(SendSequenced(peerNodeId));
        outbound.SendAck(messageTypeId, CanAckStatus::success);
    }

    // === Sending while bound ===

    TEST_F(CanCategoryOutboundTest, Send_AddressesThisNodeWithTheBoundCategory)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(outbound.Send(CanPriority::telemetry, messageTypeId, MakePayload(3)));

        ASSERT_EQ(sentIds.size(), 1u);
        EXPECT_EQ(ExtractCanPriority(sentIds[0]), CanPriority::telemetry);
        EXPECT_EQ(ExtractCanCategory(sentIds[0]), boundCategoryId);
        EXPECT_EQ(ExtractCanMessageType(sentIds[0]), messageTypeId);
        EXPECT_EQ(ExtractCanNodeId(sentIds[0]), ownNodeId);
        EXPECT_EQ(sentFrames[0].size(), 3u);
    }

    TEST_F(CanCategoryOutboundTest, SendTo_AddressesTheTargetNode)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(outbound.SendTo(peerNodeId, CanPriority::command, messageTypeId, MakePayload(3)));

        ASSERT_EQ(sentIds.size(), 1u);
        EXPECT_EQ(ExtractCanNodeId(sentIds[0]), peerNodeId);
        EXPECT_EQ(ExtractCanCategory(sentIds[0]), boundCategoryId);
        EXPECT_EQ(sentFrames[0].size(), 3u);
    }

    TEST_F(CanCategoryOutboundTest, SendAck_CorrelatesToTheRequestBeingServed)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);
        outbound.BeginRequest(peerNodeId, 0x42);

        outbound.SendAck(messageTypeId, CanAckStatus::invalidState);

        EXPECT_EQ(outbound.CurrentRequest().peerNodeId, peerNodeId);
        EXPECT_EQ(outbound.CurrentRequest().correlation, 0x42);
        ASSERT_EQ(sentFrames.size(), 1u);
        ASSERT_EQ(sentFrames[0].size(), canCommandAckSize);
        EXPECT_EQ(sentFrames[0][0], boundCategoryId);
        EXPECT_EQ(sentFrames[0][1], messageTypeId);
        EXPECT_EQ(sentFrames[0][2], static_cast<uint8_t>(CanAckStatus::invalidState));
        EXPECT_EQ(sentFrames[0][3], 0x42);
        EXPECT_EQ(sentFrames[0][4], 0);
    }

    TEST_F(CanCategoryOutboundTest, SendAck_FollowsTheRequestBeingServed)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        outbound.BeginRequest(peerNodeId, 0x42);
        outbound.BeginRequest(otherPeerNodeId, 0x07);
        outbound.SendAck(messageTypeId, CanAckStatus::success);

        ASSERT_EQ(sentFrames.size(), 1u);
        EXPECT_EQ(sentFrames[0][3], 0x07);
    }

    TEST_F(CanCategoryOutboundTest, SendAckFor_CorrelatesToTheCapturedRequest)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        outbound.BeginRequest(peerNodeId, 0x42);
        auto deferred = outbound.CurrentRequest();

        // The host serves the next request before the category answers the one
        // it captured.
        outbound.BeginRequest(otherPeerNodeId, 0x07);
        outbound.SendAckFor(deferred, messageTypeId, CanAckStatus::success);

        ASSERT_EQ(sentFrames.size(), 1u);
        ASSERT_EQ(sentFrames[0].size(), canCommandAckSize);
        EXPECT_EQ(sentFrames[0][0], boundCategoryId);
        EXPECT_EQ(sentFrames[0][1], messageTypeId);
        EXPECT_EQ(sentFrames[0][2], static_cast<uint8_t>(CanAckStatus::success));
        EXPECT_EQ(sentFrames[0][3], 0x42);
        EXPECT_EQ(sentFrames[0][4], 0);
        EXPECT_EQ(ExtractCanNodeId(sentIds[0]), ownNodeId);
    }

    // === SendSequencedTo ===

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_PrependsTheAllocatedSequence)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId, 2));
        EXPECT_TRUE(SendSequenced(peerNodeId, 2));

        ASSERT_EQ(sentFrames.size(), 2u);
        ASSERT_EQ(sentFrames[0].size(), 3u);
        EXPECT_EQ(sentFrames[0][0], 0);
        EXPECT_EQ(sentFrames[0][1], 0xB0);
        EXPECT_EQ(sentFrames[0][2], 0xB1);
        EXPECT_EQ(sentFrames[1][0], 1);
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_CountsPerPeer)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId));
        EXPECT_TRUE(SendSequenced(otherPeerNodeId));
        EXPECT_TRUE(SendSequenced(peerNodeId));
        EXPECT_TRUE(SendSequenced(otherPeerNodeId));

        ASSERT_EQ(sentFrames.size(), 4u);
        EXPECT_EQ(sentFrames[0][0], 0);
        EXPECT_EQ(sentFrames[1][0], 0);
        EXPECT_EQ(sentFrames[2][0], 1);
        EXPECT_EQ(sentFrames[3][0], 1);
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_AcceptsPayloadThatLeavesRoomForTheSequence)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId, MaxFrameSize() - 1));

        ASSERT_EQ(sentFrames.size(), 1u);
        EXPECT_EQ(sentFrames[0].size(), MaxFrameSize());
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_RefusesFullPayloadWithoutConsumingASequence)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_FALSE(SendSequenced(peerNodeId, MaxFrameSize()));
        EXPECT_TRUE(sentFrames.empty());

        EXPECT_TRUE(SendSequenced(peerNodeId));
        ASSERT_EQ(sentFrames.size(), 1u);
        EXPECT_EQ(sentFrames[0][0], 0);
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_RefusesBroadcastWithoutEmittingAFrame)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_FALSE(SendSequenced(canBroadcastNodeId));

        EXPECT_TRUE(sentFrames.empty());
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_RefusesBroadcastWithoutConsumingASequence)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_FALSE(SendSequenced(canBroadcastNodeId));

        // Nothing was allocated for the broadcast peer, so validation still
        // adopts whatever sequence arrives first for it.
        auto validation = outbound.ValidateSequence(canBroadcastNodeId, 9);
        EXPECT_TRUE(validation.accepted);
        EXPECT_EQ(validation.expected, 9);
    }

    TEST_F(CanCategoryOutboundTest, SendSequencedTo_RefusedBroadcastLeavesUnicastStreamUntouched)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId));
        EXPECT_FALSE(SendSequenced(canBroadcastNodeId));
        EXPECT_TRUE(SendSequenced(peerNodeId));

        ASSERT_EQ(sentFrames.size(), 2u);
        EXPECT_EQ(sentFrames[0][0], 0);
        EXPECT_EQ(sentFrames[1][0], 1);
    }

    TEST_F(CanCategoryOutboundTest, Unbind_ForgetsSequenceState)
    {
        RecordSentFrames();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId));
        EXPECT_TRUE(SendSequenced(peerNodeId));

        outbound.Unbind();
        outbound.Bind(transport, boundCategoryId);

        EXPECT_TRUE(SendSequenced(peerNodeId));

        ASSERT_EQ(sentFrames.size(), 3u);
        EXPECT_EQ(sentFrames[2][0], 0);
    }
}
