// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/core/quic_utils.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "base/containers/adapters.h"
#include "base/logging.h"
#include "net/quic/core/quic_constants.h"
#include "net/quic/core/quic_flags.h"
 

// HONESTCHOI added for debugging            
#include "net/quic/platform/api/quic_logging.h"
#include <execinfo.h> // honest_print_backtrace()
#include <iomanip>
extern char g_honest_buf[HONEST_DBG_MSG_BUF_SIZE];
extern uint64_t g_honest_buf_idx;

using std::string;
using namespace std;

uint32_t net::QuicUtils::honest_DefaultMaxPacketSize = 0;
uint32_t net::QuicUtils::honest_MaxPacketSize = 0;
uint32_t net::QuicUtils::honest_MtuDiscoveryTargetPacketSizeHigh = 0;
uint32_t net::QuicUtils::honest_MtuDiscoveryTargetPacketSizeLow = 0;
uint32_t net::QuicUtils::honest_DefaultNumConnections = 0;
float net::QuicUtils::honest_PacingRate = 0.0;
int net::QuicUtils::honest_UsingPacing = 0.0;
uint32_t net::QuicUtils::honest_Granularity = 0;
uint32_t net::QuicUtils::honest_ExperimentSeq = 0; 
char net::QuicUtils::honest_ProcessName[HONEST_MAX_FILE_NAME] = {0,};
int32_t net::QuicUtils::honest_UsingHonestFatal = 1;

namespace net {
namespace {

// We know that >= GCC 4.8 and Clang have a __uint128_t intrinsic. Other
// compilers don't necessarily, notably MSVC.
#if defined(__x86_64__) &&                                         \
    ((defined(__GNUC__) &&                                         \
      (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))) || \
     defined(__clang__))
#define QUIC_UTIL_HAS_UINT128 1
#endif

#ifdef QUIC_UTIL_HAS_UINT128
uint128 IncrementalHashFast(uint128 uhash, QuicStringPiece data) {
  // This code ends up faster than the naive implementation for 2 reasons:
  // 1. uint128 from base/int128.h is sufficiently complicated that the compiler
  //    cannot transform the multiplication by kPrime into a shift-multiply-add;
  //    it has go through all of the instructions for a 128-bit multiply.
  // 2. Because there are so fewer instructions (around 13), the hot loop fits
  //    nicely in the instruction queue of many Intel CPUs.
  // kPrime = 309485009821345068724781371
  static const __uint128_t kPrime =
      (static_cast<__uint128_t>(16777216) << 64) + 315;
  __uint128_t xhash = (static_cast<__uint128_t>(Uint128High64(uhash)) << 64) +
                      Uint128Low64(uhash);
  const uint8_t* octets = reinterpret_cast<const uint8_t*>(data.data());
  for (size_t i = 0; i < data.length(); ++i) {
    xhash = (xhash ^ octets[i]) * kPrime;
  }
  return MakeUint128(
      static_cast<uint64_t>(xhash >> 64),
      static_cast<uint64_t>(xhash & UINT64_C(0xFFFFFFFFFFFFFFFF)));
}
#endif

#ifndef QUIC_UTIL_HAS_UINT128
// Slow implementation of IncrementalHash. In practice, only used by Chromium.
uint128 IncrementalHashSlow(uint128 hash, QuicStringPiece data) {
  // kPrime = 309485009821345068724781371
  static const uint128 kPrime = MakeUint128(16777216, 315);
  const uint8_t* octets = reinterpret_cast<const uint8_t*>(data.data());
  for (size_t i = 0; i < data.length(); ++i) {
    hash = hash ^ MakeUint128(0, octets[i]);
    hash = hash * kPrime;
  }
  return hash;
}
#endif

uint128 IncrementalHash(uint128 hash, QuicStringPiece data) {
#ifdef QUIC_UTIL_HAS_UINT128
  return IncrementalHashFast(hash, data);
#else
  return IncrementalHashSlow(hash, data);
#endif
}

}  // namespace

// static
uint64_t QuicUtils::FNV1a_64_Hash(QuicStringPiece data) {
  static const uint64_t kOffset = UINT64_C(14695981039346656037);
  static const uint64_t kPrime = UINT64_C(1099511628211);

  const uint8_t* octets = reinterpret_cast<const uint8_t*>(data.data());

  uint64_t hash = kOffset;

  for (size_t i = 0; i < data.length(); ++i) {
    hash = hash ^ octets[i];
    hash = hash * kPrime;
  }

  return hash;
}

// static
uint128 QuicUtils::FNV1a_128_Hash(QuicStringPiece data) {
  return FNV1a_128_Hash_Three(data, QuicStringPiece(), QuicStringPiece());
}

// static
uint128 QuicUtils::FNV1a_128_Hash_Two(QuicStringPiece data1,
                                      QuicStringPiece data2) {
  return FNV1a_128_Hash_Three(data1, data2, QuicStringPiece());
}

// static
uint128 QuicUtils::FNV1a_128_Hash_Three(QuicStringPiece data1,
                                        QuicStringPiece data2,
                                        QuicStringPiece data3) {
  // The two constants are defined as part of the hash algorithm.
  // see http://www.isthe.com/chongo/tech/comp/fnv/
  // kOffset = 144066263297769815596495629667062367629
  const uint128 kOffset =
      MakeUint128(UINT64_C(7809847782465536322), UINT64_C(7113472399480571277));

  uint128 hash = IncrementalHash(kOffset, data1);
  if (data2.empty()) {
    return hash;
  }

  hash = IncrementalHash(hash, data2);
  if (data3.empty()) {
    return hash;
  }
  return IncrementalHash(hash, data3);
}

// static
void QuicUtils::SerializeUint128Short(uint128 v, uint8_t* out) {
  const uint64_t lo = Uint128Low64(v);
  const uint64_t hi = Uint128High64(v);
  // This assumes that the system is little-endian.
  memcpy(out, &lo, sizeof(lo));
  memcpy(out + sizeof(lo), &hi, sizeof(hi) / 2);
}

#define RETURN_STRING_LITERAL(x) \
  case x:                        \
    return #x;

// static
const char* QuicUtils::EncryptionLevelToString(EncryptionLevel level) {
  switch (level) {
    RETURN_STRING_LITERAL(ENCRYPTION_NONE);
    RETURN_STRING_LITERAL(ENCRYPTION_INITIAL);
    RETURN_STRING_LITERAL(ENCRYPTION_FORWARD_SECURE);
    RETURN_STRING_LITERAL(NUM_ENCRYPTION_LEVELS);
  }
  return "INVALID_ENCRYPTION_LEVEL";
}

// static
const char* QuicUtils::TransmissionTypeToString(TransmissionType type) {
  switch (type) {
    RETURN_STRING_LITERAL(NOT_RETRANSMISSION);
    RETURN_STRING_LITERAL(HANDSHAKE_RETRANSMISSION);
    RETURN_STRING_LITERAL(LOSS_RETRANSMISSION);
    RETURN_STRING_LITERAL(ALL_UNACKED_RETRANSMISSION);
    RETURN_STRING_LITERAL(ALL_INITIAL_RETRANSMISSION);
    RETURN_STRING_LITERAL(RTO_RETRANSMISSION);
    RETURN_STRING_LITERAL(TLP_RETRANSMISSION);
  }
  return "INVALID_TRANSMISSION_TYPE";
}

string QuicUtils::PeerAddressChangeTypeToString(PeerAddressChangeType type) {
  switch (type) {
    RETURN_STRING_LITERAL(NO_CHANGE);
    RETURN_STRING_LITERAL(PORT_CHANGE);
    RETURN_STRING_LITERAL(IPV4_SUBNET_CHANGE);
    RETURN_STRING_LITERAL(IPV4_TO_IPV6_CHANGE);
    RETURN_STRING_LITERAL(IPV6_TO_IPV4_CHANGE);
    RETURN_STRING_LITERAL(IPV6_TO_IPV6_CHANGE);
    RETURN_STRING_LITERAL(IPV4_TO_IPV4_CHANGE);
  }
  return "INVALID_PEER_ADDRESS_CHANGE_TYPE";
}

// static
PeerAddressChangeType QuicUtils::DetermineAddressChangeType(
    const QuicSocketAddress& old_address,
    const QuicSocketAddress& new_address) {
  if (!old_address.IsInitialized() || !new_address.IsInitialized() ||
      old_address == new_address) {
    return NO_CHANGE;
  }

  if (old_address.host() == new_address.host()) {
    return PORT_CHANGE;
  }

  bool old_ip_is_ipv4 = old_address.host().IsIPv4() ? true : false;
  bool migrating_ip_is_ipv4 = new_address.host().IsIPv4() ? true : false;
  if (old_ip_is_ipv4 && !migrating_ip_is_ipv4) {
    return IPV4_TO_IPV6_CHANGE;
  }

  if (!old_ip_is_ipv4) {
    return migrating_ip_is_ipv4 ? IPV6_TO_IPV4_CHANGE : IPV6_TO_IPV6_CHANGE;
  }

  const int kSubnetMaskLength = 24;
  if (old_address.host().InSameSubnet(new_address.host(), kSubnetMaskLength)) {
    // Subnet part does not change (here, we use /24), which is considered to be
    // caused by NATs.
    return IPV4_SUBNET_CHANGE;
  }

  return IPV4_TO_IPV4_CHANGE;
}

// HONESTCHOI added it for debugging 
void QuicUtils::honest_print_backtrace(const std::string func_name)
{
    printf("[%s][%d] called by [%s]\n", __FUNCTION__, __LINE__, func_name.c_str());
    int j, nptrs;
#define SIZE 100
    void* buffer[100];
    char **strings;
    nptrs = backtrace(buffer, SIZE);

    strings = backtrace_symbols(buffer, nptrs);
    if(!strings) {
        printf("[%s][%d] ERROR on BACKTRACE\n", __FUNCTION__, __LINE__);
        return;
    }

    for(j = 0; j < nptrs;j++)
        printf("%s\n", strings[j]);

    free(strings);
}

// extern QuicByteCount net::kDefaultMaxPacketSize;
// HONEST_CONF
int QuicUtils::honest_conf_setup(void)
{
  FILE* fp = fopen("honest.conf", "r");
  if(!fp)
    return -1;

  const int honest_entity_max_size = 1024;
  char honest_entity[honest_entity_max_size];
  float honest_value = 0.0;

  const char* DMPS = "DefaultMaxPacketSize";
  const char* MPS = "MaxPacketSize";
  const char* MDTSH = "MtuDiscoveryTargetPacketSizeHigh";
  const char* MDTSL = "MtuDiscoveryTargetPacketSizeLow";
  const char* DNC = "DefaultNumConnections";
  const char* PR = "PacingRate";
  const char* UP = "UsingPacing";
  const char* GRA = "Granularity";

  // Variables included in header file is re-written, others is written in custom global-static variable
  while(1) {
    if(EOF == fscanf(fp,"%s %f", honest_entity, &honest_value))
       break;

    if(!strncmp(honest_entity, DMPS, honest_entity_max_size)) {
      QuicUtils::honest_DefaultMaxPacketSize = honest_value;
//      net::kDefaultMaxPacketSize = honest_value;                              // quic_constant.h
    } else if(!strncmp(honest_entity, MPS, honest_entity_max_size)) {
      QuicUtils::honest_MaxPacketSize = honest_value;  // kMaxPacketSize in quic_constants.h are referred wildey, won't be modified.
    } else if(!strncmp(honest_entity, MDTSH, honest_entity_max_size)) {
      QuicUtils::honest_MtuDiscoveryTargetPacketSizeHigh = honest_value; // quic_connection.h
    } else if(!strncmp(honest_entity, MDTSL, honest_entity_max_size)) {
      QuicUtils::honest_MtuDiscoveryTargetPacketSizeLow = honest_value;  // quic_connection.h
    } else if(!strncmp(honest_entity, DNC, honest_entity_max_size)) {
      QuicUtils::honest_DefaultNumConnections = honest_value;            // cubic_bytes.cc
    } else if(!strncmp(honest_entity, PR, honest_entity_max_size)) {
      QuicUtils::honest_PacingRate = honest_value;                       // tcp_cubic_sender_base.cc
    } else if(!strncmp(honest_entity, UP, honest_entity_max_size)) {
      QuicUtils::honest_UsingPacing = honest_value;                      // tcp_cubic_sender_base.cc
    } else if(!strncmp(honest_entity, GRA, honest_entity_max_size)) {
      QuicUtils::honest_Granularity = (uint32_t)honest_value;                      // pacing_sender.cc
    }
  }

  printf("honest_DefaultMaxPacketSize:%d,\n" 
         "honest_MaxPacketSize:%d,\n" 
         "honest_MtuDiscoveryTargetPacketSizeHigh:%d,\n" 
         "honest_MtuDiscoveryTargetPacketSizeLow:%d,\n" 
         "honest_DefaultNumConnections:%d,\n" 
         "honest_PacingRate:%f,\n" 
         "honest_UsingPacing:%d\n"
         "honest_Granularity:%u\n",
    QuicUtils::honest_DefaultMaxPacketSize,
    QuicUtils::honest_MaxPacketSize,
    QuicUtils::honest_MtuDiscoveryTargetPacketSizeHigh,
    QuicUtils::honest_MtuDiscoveryTargetPacketSizeLow,
    QuicUtils::honest_DefaultNumConnections,
    QuicUtils::honest_PacingRate,
    QuicUtils::honest_UsingPacing,
    QuicUtils::honest_Granularity
  );

  HONEST_FATAL << "honest_DefaultMaxPacketSize:" << QuicUtils::honest_DefaultMaxPacketSize;
  HONEST_FATAL << "honest_MaxPacketSize:" << QuicUtils::honest_MaxPacketSize;
  HONEST_FATAL << "honest_MtuDiscoveryTargetPacketSizeHigh:" << QuicUtils::honest_MtuDiscoveryTargetPacketSizeHigh;
  HONEST_FATAL << "honest_MtuDiscoveryTargetPacketSizeLow:" << QuicUtils::honest_MtuDiscoveryTargetPacketSizeLow;
  HONEST_FATAL << "honest_DefaultNumConnections:" << QuicUtils::honest_DefaultNumConnections;
  HONEST_FATAL << "honest_PacingRate:" << QuicUtils::honest_PacingRate;
  HONEST_FATAL << "honest_UsingPacing:" << QuicUtils::honest_UsingPacing;
  HONEST_FATAL << "honest_Granularity:" << QuicUtils::honest_Granularity;
  return 1;
}

// HONEST added below for debugging
void QuicUtils::honest_sigint_handler(int s)
{
  g_honest_buf[g_honest_buf_idx++] = '\n';
  g_honest_buf[g_honest_buf_idx] = 0; 
  // Call me maybe ~! 
  // ProcessName-MM-DD-SEQ-HH-MM-NumCon_2-PacingRate_1.25-UsingPacing_1-Gra_1-DMPS_1450-MPS_1530-MDTPSH_1450-MDTPSL-1430
  std::ostringstream file_name;
  std::string tmp = QuicUtils::honest_ProcessName;
  std::string process_name = tmp.substr(tmp.find_last_of('/')+1);
  timeval tv;
  gettimeofday(&tv, nullptr);
  time_t t = tv.tv_sec;
  struct tm local_time;
  localtime_r(&t, &local_time);
  struct tm* tm_time = &local_time;
  file_name << process_name.c_str()
            << '-'
	        << std::setfill('0')
            << std::setw(2) << 1 + tm_time->tm_mon
            << std::setw(2) << tm_time->tm_mday
            << '-'
            << std::setw(2) << honest_ExperimentSeq
            << '-'
            << std::setw(2) << tm_time->tm_hour
            << std::setw(2) << tm_time->tm_min
            << '-'
			<< "NumCon_" << QuicUtils::honest_DefaultNumConnections
            << '-'
			<< "PacingRate_" << QuicUtils::honest_PacingRate
            << '-'
			<< "UsingPacing_" << QuicUtils::honest_UsingPacing
            << '-'
			<< "Gra_" << QuicUtils::honest_Granularity
            << '-'
			<< "DMPS_" << QuicUtils::honest_DefaultMaxPacketSize
            << '-'
			<< "MPS_" << QuicUtils::honest_MaxPacketSize
            << '-'
			<< "MDTPSH_" << QuicUtils::honest_MtuDiscoveryTargetPacketSizeHigh
            << '-'
			<< "MDTPSL_" << QuicUtils::honest_MtuDiscoveryTargetPacketSizeLow
			<< ".txt" ;

  printf("file_path:%s \n", file_name.str().c_str());
  FILE* fp = fopen(file_name.str().c_str(), "w");

  if(fp) {
    fwrite(g_honest_buf, g_honest_buf_idx,1, fp);
	// fprintf(fp, "%s", g_honest_buf);
    fclose(fp);
	fp = NULL;
  } else {
	printf("fopen fails\n");
  }
  exit(1);
}

}  // namespace net
