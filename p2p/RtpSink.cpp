#include "RtpSink.h"
#include <random>
#include <chrono>

using namespace asio;
using namespace std::chrono;

static inline uint32_t GetTimestamp()
{
	auto time_point = time_point_cast<milliseconds>(high_resolution_clock::now());
	return (uint32_t)time_point.time_since_epoch().count();
}

RtpSink::RtpSink(asio::io_service& io_service)
	: io_context_(io_service)
	, io_strand_(io_service)
	, rtp_packet_(new RtpPacket)
	, fec_encoder_(new fec::FecEncoder)
{
	std::random_device rd;
	rtp_packet_->SetCSRC(rd());
}

RtpSink::~RtpSink()
{
	Close();
}

bool RtpSink::Open(uint16_t rtp_port, uint16_t rtcp_port)
{
	rtp_socket_.reset(new UdpSocket(io_context_));
	rtcp_socket_.reset(new UdpSocket(io_context_));

	if (!rtp_socket_->Open("0.0.0.0", rtp_port) ||
		!rtcp_socket_->Open("0.0.0.0", rtcp_port)) {
		rtp_socket_.reset();
		rtcp_socket_.reset();
		return false;
	}

	if (rtp_socket_) {
		std::weak_ptr<RtpSink> rtp_sink = shared_from_this();
		rtp_socket_->Receive([rtp_sink](void* data, size_t size, asio::ip::udp::endpoint& ep) {
			auto sink = rtp_sink.lock();
			if (!sink) {
				return false;
			}

			if (size == 1) {
				sink->peer_rtp_address_ = ep;
				char empty_packet[1] = { 0 };
				sink->rtp_socket_->Send(empty_packet, 1, ep);
			}

			return true;
		});
	}

	return true;
}

bool RtpSink::Open()
{
	std::random_device rd;
	for (int n = 0; n <= 10; n++) {
		if (n == 10) {
			return false;
		}

		uint16_t rtp_port  = rd() & 0xfffe;
		uint16_t rtcp_port = rtp_port + 1;

		if (Open(rtp_port, rtcp_port)) {
			break;
		}
	}
	return true;
}

void RtpSink::Close()
{
	if (rtp_socket_) {
		rtp_socket_->Close();
		rtp_socket_.reset();
	}

	if (rtcp_socket_) {
		rtcp_socket_->Close();
		rtcp_socket_.reset();
	}
}

void RtpSink::SetPeerAddress(std::string ip, uint16_t rtp_port, uint16_t rtcp_port)
{
	peer_rtp_address_ = ip::udp::endpoint(ip::address_v4::from_string(ip), rtp_port);
	peer_rtcp_address_ = ip::udp::endpoint(ip::address_v4::from_string(ip), rtcp_port);
}

uint16_t RtpSink::GetRtpPort() const 
{
	if (rtp_socket_) {
		return rtp_socket_->GetLocalPoint().port();
	}

	return 0;
}

uint16_t RtpSink::GetRtcpPort() const
{
	if (rtcp_socket_) {
		return rtcp_socket_->GetLocalPoint().port();
	}

	return 0;
}

bool RtpSink::SendFrame(std::shared_ptr<uint8_t> data, uint32_t size, uint8_t type, uint32_t timestamp)
{
	if (!rtp_socket_) {
		return false;
	}

	std::weak_ptr<RtpSink> rtp_sink = shared_from_this();

	io_strand_.dispatch([rtp_sink, data, size, type, timestamp] {
		auto sink = rtp_sink.lock();
		if (sink) {
			sink->HandleFrame(data, size, type, timestamp);
		}		
	});

	return true;
}

void RtpSink::HandleFrame(std::shared_ptr<uint8_t> data, uint32_t size, uint8_t type, uint32_t timestamp)
{
	if (!rtp_socket_ || !peer_rtp_address_.port()) {
		return;
	}

	if (use_fec_ > 0 && fec_perc_ > 0) {
		return SendRtpOverFEC(data, size, type, timestamp);
	}
	else {
		return SendRtp(data, size, type, timestamp);
	}
}

void RtpSink::SendRtp(std::shared_ptr<uint8_t>& data, uint32_t& size, uint8_t& type, uint32_t& timestamp)
{
	int rtp_payload_size = packet_size_ - RTP_HEADER_SIZE;
	int data_index = 0;
	int data_size = size;

	rtp_packet_->SetPayloadType(type);
	rtp_packet_->SetTimestamp(timestamp);
	rtp_packet_->SetMarker(0);

	while (data_index < data_size) {
		int bytes_used = data_size - data_index;
		if (bytes_used > rtp_payload_size) {
			bytes_used = rtp_payload_size;
		}
		else {
			rtp_packet_->SetMarker(1);
		}

		rtp_packet_->SetSeq(packet_seq_++);
		rtp_packet_->SetPayload(data.get() + data_index, bytes_used);
		data_index += bytes_used;
		rtp_socket_->Send(rtp_packet_->Get(), rtp_packet_->Size(), peer_rtp_address_);
		//printf("mark:%d, seq:%d, size: %u\n", rtp_packet_->GetMarker(), rtp_packet_->GetSeq(), rtp_packet_->Size());
	}
}

void RtpSink::SendRtpOverFEC(std::shared_ptr<uint8_t>& data, uint32_t& size, uint8_t& type, uint32_t& timestamp)
{
	int rtp_payload_size = packet_size_ - RTP_HEADER_SIZE;
	int data_index = 0;
	int data_size = size;

	rtp_packet_->SetPayloadType(type);
	rtp_packet_->SetTimestamp(timestamp);
	rtp_packet_->SetMarker(0);

	rtp_packet_->SetExtension(1); // use fec

	fec_encoder_->SetPercentage(fec_perc_);
	fec_encoder_->SetPacketSize(rtp_payload_size);

	fec::FecPackets out_packets;
	int ret = fec_encoder_->Encode(data.get(), size, out_packets);
	if (ret != 0) {
		return;
	}

	for (auto iter : out_packets) {
		if(iter == *out_packets.rbegin()) {
			rtp_packet_->SetMarker(1);
		}

		rtp_packet_->SetSeq(packet_seq_++);
		rtp_packet_->SetPayload((uint8_t*)iter.second.get(), sizeof(fec::FecPacket));

		bool loss = packet_loss_perc_ > 0 && (rand() % 100 < packet_loss_perc_);
		if (!loss) {
			rtp_socket_->Send(rtp_packet_->Get(), rtp_packet_->Size(), peer_rtp_address_);
			//printf("mark:%d, seq:%d, size: %u\n", rtp_packet_->GetMarker(), rtp_packet_->GetSeq(), rtp_packet_->Size());
		}		
	}
}