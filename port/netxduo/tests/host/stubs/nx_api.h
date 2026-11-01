/* Minimal NetX Duo stub for workspace compile checks and host tests (RA8FW-318). */
#ifndef NXSTUB_NX_API_H
#define NXSTUB_NX_API_H
#include <stdint.h>
typedef unsigned char UCHAR; typedef unsigned int UINT; typedef unsigned long ULONG; typedef long LONG; typedef char CHAR;
typedef struct NX_PACKET_POOL_STRUCT { UCHAR* nx_packet_pool_name; } NX_PACKET_POOL;
typedef struct NX_PACKET_STRUCT {
  struct NX_PACKET_STRUCT* nx_packet_next;
  UCHAR* nx_packet_prepend_ptr; UCHAR* nx_packet_append_ptr;
  ULONG nx_packet_length; struct NX_INTERFACE_STRUCT* nx_packet_ip_interface;
} NX_PACKET;
typedef struct NX_INTERFACE_STRUCT {
  ULONG nx_interface_physical_address_msw; ULONG nx_interface_physical_address_lsw;
  ULONG nx_interface_ip_mtu_size; UINT nx_interface_address_mapping_needed; UINT nx_interface_link_up;
} NX_INTERFACE;
typedef struct NX_IP_STRUCT { NX_PACKET_POOL* nx_ip_default_packet_pool; } NX_IP;
typedef struct NX_IP_DRIVER_STRUCT {
  UINT nx_ip_driver_command; NX_IP* nx_ip_driver_ptr; NX_PACKET* nx_ip_driver_packet;
  NX_INTERFACE* nx_ip_driver_interface; ULONG nx_ip_driver_physical_address_msw;
  ULONG nx_ip_driver_physical_address_lsw; ULONG* nx_ip_driver_return_ptr; UINT nx_ip_driver_status;
} NX_IP_DRIVER;
#define NX_LINK_INITIALIZE 1
#define NX_LINK_UNINITIALIZE 2
#define NX_LINK_ENABLE 3
#define NX_LINK_DISABLE 4
#define NX_LINK_PACKET_SEND 5
#define NX_LINK_PACKET_BROADCAST 6
#define NX_LINK_ARP_SEND 7
#define NX_LINK_ARP_RESPONSE_SEND 8
#define NX_LINK_RARP_SEND 9
#define NX_LINK_MULTICAST_JOIN 10
#define NX_LINK_MULTICAST_LEAVE 11
#define NX_LINK_INTERFACE_ATTACH 12
#define NX_LINK_INTERFACE_DETACH 13
#define NX_LINK_SET_PHYSICAL_ADDRESS 14
#define NX_LINK_DEFERRED_PROCESSING 15
#define NX_LINK_GET_STATUS 16
#define NX_SUCCESS 0
#define NX_NOT_SUCCESSFUL 1
#define NX_TRUE 1
#define NX_FALSE 0
#define NX_NO_WAIT 0
#define NX_NULL ((void*)0)
#define NX_RECEIVE_PACKET 2
UINT nx_packet_allocate(NX_PACKET_POOL*, NX_PACKET**, UINT, ULONG);
UINT nx_packet_data_append(NX_PACKET*, void*, ULONG, NX_PACKET_POOL*, ULONG);
UINT nx_packet_release(NX_PACKET*);
UINT nx_packet_transmit_release(NX_PACKET*);
void _nx_ip_packet_deferred_receive(NX_IP*, NX_PACKET*);
#endif
