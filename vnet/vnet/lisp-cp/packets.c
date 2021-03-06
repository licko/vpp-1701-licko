/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vnet/lisp-cp/packets.h>
#include <vnet/lisp-cp/lisp_cp_messages.h>
#include <vnet/ip/udp_packet.h>

/* Returns IP ID for the packet */
/* static u16 ip_id = 0;
static inline u16
get_IP_ID()
{
    ip_id++;
    return (ip_id);
} */

u16
udp_ip4_checksum (const void *b, u32 len, u8 * src, u8 * dst)
{
  const u16 *buf = b;
  u16 *ip_src = (u16 *) src;
  u16 *ip_dst = (u16 *) dst;
  u32 length = len;
  u32 sum = 0;

  while (len > 1)
    {
      sum += *buf++;
      if (sum & 0x80000000)
	sum = (sum & 0xFFFF) + (sum >> 16);
      len -= 2;
    }

  /* Add the padding if the packet length is odd */
  if (len & 1)
    sum += *((u8 *) buf);

  /* Add the pseudo-header */
  sum += *(ip_src++);
  sum += *ip_src;

  sum += *(ip_dst++);
  sum += *ip_dst;

  sum += clib_host_to_net_u16 (IP_PROTOCOL_UDP);
  sum += clib_host_to_net_u16 (length);

  /* Add the carries */
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);

  /* Return the one's complement of sum */
  return ((u16) (~sum));
}

u16
udp_ip6_checksum (ip6_header_t * ip6, udp_header_t * up, u32 len)
{
  size_t i;
  register const u16 *sp;
  u32 sum;
  union
  {
    struct
    {
      ip6_address_t ph_src;
      ip6_address_t ph_dst;
      u32 ph_len;
      u8 ph_zero[3];
      u8 ph_nxt;
    } ph;
    u16 pa[20];
  } phu;

  /* pseudo-header */
  memset (&phu, 0, sizeof (phu));
  phu.ph.ph_src = ip6->src_address;
  phu.ph.ph_dst = ip6->dst_address;
  phu.ph.ph_len = clib_host_to_net_u32 (len);
  phu.ph.ph_nxt = IP_PROTOCOL_UDP;

  sum = 0;
  for (i = 0; i < sizeof (phu.pa) / sizeof (phu.pa[0]); i++)
    sum += phu.pa[i];

  sp = (const u16 *) up;

  for (i = 0; i < (len & ~1); i += 2)
    sum += *sp++;

  if (len & 1)
    sum += clib_host_to_net_u16 ((*(const u8 *) sp) << 8);

  while (sum > 0xffff)
    sum = (sum & 0xffff) + (sum >> 16);
  sum = ~sum & 0xffff;

  return (sum);
}

u16
udp_checksum (udp_header_t * uh, u32 udp_len, void *ih, u8 version)
{
  switch (version)
    {
    case IP4:
      return (udp_ip4_checksum (uh, udp_len,
				((ip4_header_t *) ih)->src_address.as_u8,
				((ip4_header_t *) ih)->dst_address.as_u8));
    case IP6:
      return (udp_ip6_checksum (ih, uh, udp_len));
    default:
      return ~0;
    }
}

void *
pkt_push_udp (vlib_main_t * vm, vlib_buffer_t * b, u16 sp, u16 dp)
{
  udp_header_t *uh;
  u16 udp_len = sizeof (udp_header_t) + vlib_buffer_length_in_chain (vm, b);

  uh = vlib_buffer_push_uninit (b, sizeof (*uh));

  uh->src_port = clib_host_to_net_u16 (sp);
  uh->dst_port = clib_host_to_net_u16 (dp);
  uh->length = clib_host_to_net_u16 (udp_len);
  uh->checksum = 0;
  return uh;
}

void *
pkt_push_ipv4 (vlib_main_t * vm, vlib_buffer_t * b, ip4_address_t * src,
	       ip4_address_t * dst, int proto)
{
  ip4_header_t *ih;

  /* make some room */
  ih = vlib_buffer_push_uninit (b, sizeof (ip4_header_t));

  ih->ip_version_and_header_length = 0x45;
  ih->tos = 0;
  ih->length = clib_host_to_net_u16 (vlib_buffer_length_in_chain (vm, b));

  /* iph->fragment_id = clib_host_to_net_u16(get_IP_ID ()); */

  /* TODO: decide if we allow fragments in case of control */
  ih->flags_and_fragment_offset = clib_host_to_net_u16 (IP_DF);
  ih->ttl = 255;
  ih->protocol = proto;
  ih->src_address.as_u32 = src->as_u32;
  ih->dst_address.as_u32 = dst->as_u32;

  ih->checksum = ip4_header_checksum (ih);
  return ih;
}

void *
pkt_push_ipv6 (vlib_main_t * vm, vlib_buffer_t * b, ip6_address_t * src,
	       ip6_address_t * dst, int proto)
{
  ip6_header_t *ip6h;
  u16 payload_length;

  /* make some room */
  ip6h = vlib_buffer_push_uninit (b, sizeof (ip6_header_t));

  ip6h->ip_version_traffic_class_and_flow_label =
    clib_host_to_net_u32 (0x6 << 28);

  /* calculate ip6 payload length */
  payload_length = vlib_buffer_length_in_chain (vm, b);
  payload_length -= sizeof (*ip6h);

  ip6h->payload_length = clib_host_to_net_u16 (payload_length);

  ip6h->hop_limit = 0xff;
  ip6h->protocol = proto;
  clib_memcpy (ip6h->src_address.as_u8, src->as_u8,
	       sizeof (ip6h->src_address));
  clib_memcpy (ip6h->dst_address.as_u8, dst->as_u8,
	       sizeof (ip6h->src_address));

  return ip6h;
}

void *
pkt_push_ip (vlib_main_t * vm, vlib_buffer_t * b, ip_address_t * src,
	     ip_address_t * dst, u32 proto)
{
  if (ip_addr_version (src) != ip_addr_version (dst))
    {
      clib_warning ("src %U and dst %U IP have different AFI! Discarding!",
		    format_ip_address, src, format_ip_address, dst);
      return 0;
    }

  switch (ip_addr_version (src))
    {
    case IP4:
      return pkt_push_ipv4 (vm, b, &ip_addr_v4 (src), &ip_addr_v4 (dst),
			    proto);
      break;
    case IP6:
      return pkt_push_ipv6 (vm, b, &ip_addr_v6 (src), &ip_addr_v6 (dst),
			    proto);
      break;
    }

  return 0;
}

void *
pkt_push_udp_and_ip (vlib_main_t * vm, vlib_buffer_t * b, u16 sp, u16 dp,
		     ip_address_t * sip, ip_address_t * dip)
{
  u16 udpsum;
  udp_header_t *uh;
  void *ih;

  uh = pkt_push_udp (vm, b, sp, dp);

  ih = pkt_push_ip (vm, b, sip, dip, IP_PROTOCOL_UDP);

  udpsum = udp_checksum (uh, clib_net_to_host_u16 (uh->length), ih,
			 ip_addr_version (sip));
  if (udpsum == (u16) ~ 0)
    {
      clib_warning ("Failed UDP checksum! Discarding");
      return 0;
    }
  uh->checksum = udpsum;
  return ih;
}

void *
pkt_push_ecm_hdr (vlib_buffer_t * b)
{
  ecm_hdr_t *h;
  h = vlib_buffer_push_uninit (b, sizeof (h[0]));

  memset (h, 0, sizeof (h[0]));
  h->type = LISP_ENCAP_CONTROL_TYPE;
  memset (h->reserved2, 0, sizeof (h->reserved2));

  return h;
}

/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
