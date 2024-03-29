/*
 * libjingle
 * Copyright 2004--2005, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/p2p/base/transport.h"

#include "talk/base/common.h"
#include "talk/p2p/base/candidate.h"
#include "talk/p2p/base/constants.h"
#include "talk/p2p/base/sessionmanager.h"
#include "talk/p2p/base/parsing.h"
#include "talk/p2p/base/transportchannelimpl.h"
#include "talk/xmllite/xmlelement.h"
#include "talk/xmpp/constants.h"

namespace cricket {

struct ChannelParams {
  ChannelParams() : channel(NULL), candidate(NULL) {}
  explicit ChannelParams(const std::string& name)
      : name(name), channel(NULL), candidate(NULL) {}
  ChannelParams(const std::string& name,
                const std::string& content_type)
      : name(name), content_type(content_type),
        channel(NULL), candidate(NULL) {}
  explicit ChannelParams(cricket::Candidate* candidate) :
      channel(NULL), candidate(candidate) {
    name = candidate->name();
  }

  ~ChannelParams() {
    delete candidate;
  }

  std::string name;
  std::string content_type;
  cricket::TransportChannelImpl* channel;
  cricket::Candidate* candidate;
};
typedef talk_base::TypedMessageData<ChannelParams*> ChannelMessage;

enum {
  MSG_CREATECHANNEL = 1,
  MSG_DESTROYCHANNEL = 2,
  MSG_DESTROYALLCHANNELS = 3,
  MSG_CONNECTCHANNELS = 4,
  MSG_RESETCHANNELS = 5,
  MSG_ONSIGNALINGREADY = 6,
  MSG_ONREMOTECANDIDATE = 7,
  MSG_READSTATE = 8,
  MSG_WRITESTATE = 9,
  MSG_REQUESTSIGNALING = 10,
  MSG_ONCHANNELCANDIDATEREADY = 11,
  MSG_CONNECTING = 12,
};

Transport::Transport(talk_base::Thread* signaling_thread,
                     talk_base::Thread* worker_thread,
                     const std::string& type,
                     PortAllocator* allocator)
  : signaling_thread_(signaling_thread),
    worker_thread_(worker_thread), type_(type), allocator_(allocator),
    destroyed_(false), readable_(false), writable_(false),
    connect_requested_(false), allow_local_ips_(false) {
}

Transport::~Transport() {
  ASSERT(signaling_thread_->IsCurrent());
  ASSERT(destroyed_);
}

TransportChannelImpl* Transport::CreateChannel(
    const std::string& name, const std::string& content_type) {
  ChannelParams params(name, content_type);
  ChannelMessage msg(&params);
  worker_thread()->Send(this, MSG_CREATECHANNEL, &msg);
  return msg.data()->channel;
}

TransportChannelImpl* Transport::CreateChannel_w(
    const std::string& name, const std::string& content_type) {
  ASSERT(worker_thread()->IsCurrent());

  TransportChannelImpl* impl = CreateTransportChannel(name, content_type);
  impl->SignalReadableState.connect(this, &Transport::OnChannelReadableState);
  impl->SignalWritableState.connect(this, &Transport::OnChannelWritableState);
  impl->SignalRequestSignaling.connect(
      this, &Transport::OnChannelRequestSignaling);
  impl->SignalCandidateReady.connect(this, &Transport::OnChannelCandidateReady);

  talk_base::CritScope cs(&crit_);
  ASSERT(channels_.find(name) == channels_.end());
  channels_[name] = impl;
  destroyed_ = false;
  if (connect_requested_) {
    impl->Connect();
    if (channels_.size() == 1) {
      // If this is the first channel, then indicate that we have started
      // connecting.
      signaling_thread()->Post(this, MSG_CONNECTING, NULL);
    }
  }
  return impl;
}

TransportChannelImpl* Transport::GetChannel(const std::string& name) {
  talk_base::CritScope cs(&crit_);
  ChannelMap::iterator iter = channels_.find(name);
  return (iter != channels_.end()) ? iter->second : NULL;
}

bool Transport::HasChannels() {
  talk_base::CritScope cs(&crit_);
  return !channels_.empty();
}

void Transport::DestroyChannel(const std::string& name) {
  ChannelParams params(name);
  ChannelMessage msg(&params);
  worker_thread()->Send(this, MSG_DESTROYCHANNEL, &msg);
}

void Transport::DestroyChannel_w(const std::string& name) {
  ASSERT(worker_thread()->IsCurrent());
  TransportChannelImpl* impl = NULL;
  {
    talk_base::CritScope cs(&crit_);
    ChannelMap::iterator iter = channels_.find(name);
    ASSERT(iter != channels_.end());
    impl = iter->second;
    channels_.erase(iter);
  }

  if (connect_requested_ && channels_.empty()) {
    // We're no longer attempting to connect.
    signaling_thread()->Post(this, MSG_CONNECTING, NULL);
  }

  if (impl) {
    // Check in case the deleted channel was the only non-writable channel.
    OnChannelWritableState(impl);
    DestroyTransportChannel(impl);
  }
}

void Transport::ConnectChannels() {
  ASSERT(signaling_thread()->IsCurrent());
  worker_thread()->Send(this, MSG_CONNECTCHANNELS, NULL);
}

void Transport::ConnectChannels_w() {
  ASSERT(worker_thread()->IsCurrent());
  if (connect_requested_)
    return;
  connect_requested_ = true;
  signaling_thread()->Post(
      this, MSG_ONCHANNELCANDIDATEREADY, NULL);
  CallChannels_w(&TransportChannelImpl::Connect);
  if (!channels_.empty()) {
    signaling_thread()->Post(this, MSG_CONNECTING, NULL);
  }
}

void Transport::OnConnecting_s() {
  ASSERT(signaling_thread()->IsCurrent());
  SignalConnecting(this);
}

void Transport::DestroyAllChannels() {
  ASSERT(signaling_thread()->IsCurrent());
  worker_thread()->Send(this, MSG_DESTROYALLCHANNELS, NULL);
  destroyed_ = true;
}

void Transport::DestroyAllChannels_w() {
  ASSERT(worker_thread()->IsCurrent());
  std::vector<TransportChannelImpl*> impls;
  {
    talk_base::CritScope cs(&crit_);
    for (ChannelMap::iterator iter = channels_.begin();
         iter != channels_.end();
         ++iter) {
      impls.push_back(iter->second);
    }
    channels_.clear();
  }

  for (size_t i = 0; i < impls.size(); ++i)
    DestroyTransportChannel(impls[i]);
}

void Transport::ResetChannels() {
  ASSERT(signaling_thread()->IsCurrent());
  worker_thread()->Send(this, MSG_RESETCHANNELS, NULL);
}

void Transport::ResetChannels_w() {
  ASSERT(worker_thread()->IsCurrent());

  // We are no longer attempting to connect
  connect_requested_ = false;

  // Clear out the old messages, they aren't relevant
  talk_base::CritScope cs(&crit_);
  ready_candidates_.clear();

  // Reset all of the channels
  CallChannels_w(&TransportChannelImpl::Reset);
}

void Transport::OnSignalingReady() {
  ASSERT(signaling_thread()->IsCurrent());
  worker_thread()->Post(this, MSG_ONSIGNALINGREADY, NULL);

  // Notify the subclass.
  OnTransportSignalingReady();
}

void Transport::CallChannels_w(TransportChannelFunc func) {
  ASSERT(worker_thread()->IsCurrent());
  talk_base::CritScope cs(&crit_);
  for (ChannelMap::iterator iter = channels_.begin();
       iter != channels_.end();
       ++iter) {
    ((iter->second)->*func)();
  }
}

bool Transport::VerifyCandidate(const Candidate& cand, ParseError* error) {
  if (cand.address().IsLocalIP() && !allow_local_ips_)
    return BadParse("candidate has local IP address", error);

  // No address zero.
  if (cand.address().IsAny()) {
    return BadParse("candidate has address of zero", error);
  }

  // Disallow all ports below 1024, except for 80 and 443 on public addresses.
  int port = cand.address().port();
  if (port < 1024) {
    if ((port != 80) && (port != 443))
      return BadParse(
          "candidate has port below 1024, but not 80 or 443", error);
    if (cand.address().IsPrivateIP()) {
      return BadParse(
          "candidate has port of 80 or 443 with private IP address", error);
    }
  }

  return true;
}

void Transport::OnRemoteCandidates(const std::vector<Candidate>& candidates) {
  for (std::vector<Candidate>::const_iterator iter = candidates.begin();
       iter != candidates.end();
       ++iter) {
    OnRemoteCandidate(*iter);
  }
}

void Transport::OnRemoteCandidate(const Candidate& candidate) {
  ASSERT(signaling_thread()->IsCurrent());
  ASSERT(HasChannel(candidate.name()));
  // new candidate deleted when params is deleted
  ChannelParams* params = new ChannelParams(new Candidate(candidate));
  ChannelMessage* msg = new ChannelMessage(params);
  worker_thread()->Post(this, MSG_ONREMOTECANDIDATE, msg);
}

void Transport::OnRemoteCandidate_w(const Candidate& candidate) {
  ASSERT(worker_thread()->IsCurrent());
  ChannelMap::iterator iter = channels_.find(candidate.name());
  // It's ok for a channel to go away while this message is in transit.
  if (iter != channels_.end()) {
    iter->second->OnCandidate(candidate);
  }
}

void Transport::OnChannelReadableState(TransportChannel* channel) {
  ASSERT(worker_thread()->IsCurrent());
  signaling_thread()->Post(this, MSG_READSTATE, NULL);
}

void Transport::OnChannelReadableState_s() {
  ASSERT(signaling_thread()->IsCurrent());
  bool readable = GetTransportState_s(true);
  if (readable_ != readable) {
    readable_ = readable;
    SignalReadableState(this);
  }
}

void Transport::OnChannelWritableState(TransportChannel* channel) {
  ASSERT(worker_thread()->IsCurrent());
  signaling_thread()->Post(this, MSG_WRITESTATE, NULL);
}

void Transport::OnChannelWritableState_s() {
  ASSERT(signaling_thread()->IsCurrent());
  bool writable = GetTransportState_s(false);
  if (writable_ != writable) {
    writable_ = writable;
    SignalWritableState(this);
  }
}

bool Transport::GetTransportState_s(bool read) {
  ASSERT(signaling_thread()->IsCurrent());
  bool result = false;
  talk_base::CritScope cs(&crit_);
  for (ChannelMap::iterator iter = channels_.begin();
       iter != channels_.end();
       ++iter) {
    bool b = (read ? iter->second->readable() : iter->second->writable());
    result = result || b;
  }
  return result;
}

void Transport::OnChannelRequestSignaling() {
  ASSERT(worker_thread()->IsCurrent());
  signaling_thread()->Post(this, MSG_REQUESTSIGNALING, NULL);
}

void Transport::OnChannelRequestSignaling_s() {
  ASSERT(signaling_thread()->IsCurrent());
  SignalRequestSignaling(this);
}

void Transport::OnChannelCandidateReady(TransportChannelImpl* channel,
                                        const Candidate& candidate) {
  ASSERT(worker_thread()->IsCurrent());
  talk_base::CritScope cs(&crit_);
  ready_candidates_.push_back(candidate);

  // We hold any messages until the client lets us connect.
  if (connect_requested_) {
    signaling_thread()->Post(
        this, MSG_ONCHANNELCANDIDATEREADY, NULL);
  }
}

void Transport::OnChannelCandidateReady_s() {
  ASSERT(signaling_thread()->IsCurrent());
  ASSERT(connect_requested_);

  std::vector<Candidate> candidates;
  {
    talk_base::CritScope cs(&crit_);
    candidates.swap(ready_candidates_);
  }

  // we do the deleting of Candidate* here to keep the new above and
  // delete below close to each other
  if (!candidates.empty()) {
    SignalCandidatesReady(this, candidates);
  }
}

void Transport::OnMessage(talk_base::Message* msg) {
  switch (msg->message_id) {
  case MSG_CREATECHANNEL:
    {
      ChannelParams* params = static_cast<ChannelMessage*>(msg->pdata)->data();
      params->channel = CreateChannel_w(params->name, params->content_type);
    }
    break;
  case MSG_DESTROYCHANNEL:
    {
      ChannelParams* params = static_cast<ChannelMessage*>(msg->pdata)->data();
      DestroyChannel_w(params->name);
    }
    break;
  case MSG_CONNECTCHANNELS:
    ConnectChannels_w();
    break;
  case MSG_RESETCHANNELS:
    ResetChannels_w();
    break;
  case MSG_DESTROYALLCHANNELS:
    DestroyAllChannels_w();
    break;
  case MSG_ONSIGNALINGREADY:
    CallChannels_w(&TransportChannelImpl::OnSignalingReady);
    break;
  case MSG_ONREMOTECANDIDATE:
    {
      ChannelMessage* channel_msg = static_cast<ChannelMessage*>(msg->pdata);
      ChannelParams* params = channel_msg->data();
      OnRemoteCandidate_w(*(params->candidate));
      delete params;
      delete channel_msg;
    }
    break;
  case MSG_CONNECTING:
    OnConnecting_s();
    break;
  case MSG_READSTATE:
    OnChannelReadableState_s();
    break;
  case MSG_WRITESTATE:
    OnChannelWritableState_s();
    break;
  case MSG_REQUESTSIGNALING:
    OnChannelRequestSignaling_s();
    break;
  case MSG_ONCHANNELCANDIDATEREADY:
    OnChannelCandidateReady_s();
    break;
  }
}

bool TransportParser::ParseAddress(const buzz::XmlElement* elem,
                                   const buzz::QName& address_name,
                                   const buzz::QName& port_name,
                                   talk_base::SocketAddress* address,
                                   ParseError* error) {
  if (!elem->HasAttr(address_name))
    return BadParse("address does not have " + address_name.LocalPart(), error);
  if (!elem->HasAttr(port_name))
    return BadParse("address does not have " + port_name.LocalPart(), error);

  address->SetIP(elem->Attr(address_name));
  std::istringstream ist(elem->Attr(port_name));
  int port;
  ist >> port;
  address->SetPort(port);

  return true;
}

}  // namespace cricket
