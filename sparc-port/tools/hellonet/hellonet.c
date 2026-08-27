/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*	The third kind of userland: one that moves a packet.
 *
 *	tools/usertest is freestanding and tests the kernel with nothing in the way.
 *	tools/hellodyn tests everything that comes after -- the loader, libroot, the
 *	relocations. This one tests the part neither of them reaches: the network
 *	stack, the `hme` driver, and the wire between them.
 *
 *	It exists because assigning an address to an interface needs a userland, and
 *	until Phase 6 closed there was not one. `hme` has attached and negotiated
 *	100baseTX-FDX since the session it was written, and has never been asked to
 *	transmit anything, because nothing could ask.
 *
 *	Five steps, each announced before it is attempted and each reporting what it
 *	got. The order matters and so does the reporting: this is the first time
 *	Haiku's network stack has run on a big-endian machine, and the failure that
 *	is coming is more likely to be a field read at the wrong end than anything
 *	structural. A program that says "step 3 returned Bad address" locates that;
 *	one that prints nothing until it succeeds does not.
 *
 *	Everything goes to _kern_debug_output(), for the reason hellodyn's comment
 *	gives: this runs as the launch daemon, before there is anything on the other
 *	end of file descriptor one.
 *
 *	The addresses are QEMU's. Its user-mode network puts the guest at 10.0.2.15
 *	behind a gateway at 10.0.2.2 which answers ICMP, so a machine that can ping
 *	10.0.2.2 has exercised ARP, IPv4, ICMP, and the driver in both directions,
 *	with no host configuration and no privileges. Real hardware wants real
 *	addresses and that is a command-line away, once there is a command line.
 */

#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <unistd.h>


extern void _kern_debug_output(const char* message);

/*	The socket calls, as syscalls rather than through libnetwork.
 *
 *	libnetwork is where socket(), sendto() and the rest live, and it does not
 *	build for this architecture yet -- its vendored NetBSD resolver sets a
 *	`__n_pad0` member that Haiku's own `struct netent` does not have. That is a
 *	real bug and someone should fix it, but fixing it is not what this program is
 *	for, and linking against it would put a second untested thing between here
 *	and the stack.
 *
 *	libroot exports the syscalls themselves, which is what libnetwork's wrappers
 *	call anyway. Declared here rather than included, for the same reason
 *	_kern_debug_output() is: they live in a private system header this program is
 *	not built with the include path for.
 *
 *	They report differently from the POSIX wrappers. A negative return *is* the
 *	error -- there is no errno -- so every call site below reads the return value
 *	rather than asking what went wrong afterwards.
 */
extern int _kern_socket(int family, int type, int protocol);
extern ssize_t _kern_sendto(int socket, const void* data, size_t length,
	int flags, const struct sockaddr* address, socklen_t addressLength);
extern ssize_t _kern_recvfrom(int socket, void* data, size_t length, int flags,
	struct sockaddr* address, socklen_t* _addressLength);
extern int _kern_setsockopt(int socket, int level, int option,
	const void* value, socklen_t length);


#define INTERFACE		"/dev/net/hme/0"
#define LOCAL_ADDRESS	"10.0.2.15"
#define NETMASK			"255.255.255.0"
#define BROADCAST		"10.0.2.255"
#define PEER_ADDRESS	"10.0.2.2"


static void
say(const char* format, ...)
{
	char buffer[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(buffer, sizeof(buffer) - 2, format, arguments);
	va_end(arguments);

	strlcat(buffer, "\n", sizeof(buffer));
	_kern_debug_output(buffer);
}


/*!	Fills a sockaddr_storage with an IPv4 address given in dotted quad.

	By hand rather than through inet_addr(), because the resolver lives in
	libnetwork and this program deliberately links against libroot alone -- the
	point is to test the stack, and a link against something that does not build
	yet would test the toolchain instead.

	The address is assembled into host order and then converted, which is the
	only place in this file where the byte order is written out rather than
	assumed. On a big-endian machine htonl() is the identity, so a bug here
	cannot show up until the same code runs somewhere else; it is spelled
	correctly anyway.
*/
static int
set_address(struct sockaddr_storage* storage, const char* dotted)
{
	unsigned int quad[4];
	if (sscanf(dotted, "%u.%u.%u.%u", &quad[0], &quad[1], &quad[2], &quad[3])
			!= 4) {
		return -1;
	}

	struct sockaddr_in* address = (struct sockaddr_in*)storage;
	memset(storage, 0, sizeof(*storage));
	address->sin_family = AF_INET;
	address->sin_len = sizeof(struct sockaddr_in);
	address->sin_addr.s_addr = htonl((quad[0] << 24) | (quad[1] << 16)
		| (quad[2] << 8) | quad[3]);

	return 0;
}


/*!	The Internet checksum, over an ICMP message this program builds itself.

	One's complement sum of 16-bit words, complemented. Written out because a raw
	ICMP socket does not fill it in -- the kernel computes the *IP* header's
	checksum and leaves the payload alone -- and because this is one of the two
	or three places where a big-endian bug would be invisible: the sum is
	endian-neutral by construction, so it is right here and would still be right
	if every other field were wrong.
*/
static uint16_t
checksum(const void* data, size_t length)
{
	const uint16_t* word = (const uint16_t*)data;
	uint32_t sum = 0;

	while (length > 1) {
		sum += *word++;
		length -= 2;
	}
	if (length > 0)
		sum += *(const uint8_t*)word;

	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)~sum;
}


/*!	Brings up the interface, and says exactly which step failed if one does.

	Three ioctls, in an order the stack requires: the interface has to exist
	before it can have an address, and it has to have one before bringing it up
	means anything.

	The first is a SIOCAIFADDR with nothing but a name in it, which is how an
	interface is created -- there is no separate "add interface" call, and
	BNetworkRoster::AddInterface() is exactly this. The name is the device's path
	under /dev, which is how Haiku names interfaces.
*/
static int
configure_interface(int socketDescriptor)
{
	struct ifaliasreq request;

	memset(&request, 0, sizeof(request));
	strlcpy(request.ifra_name, INTERFACE, IF_NAMESIZE);

	say("hellonet: creating %s", INTERFACE);
	if (ioctl(socketDescriptor, SIOCAIFADDR, &request, sizeof(request)) < 0) {
		say("hellonet: create failed: %s", strerror(errno));
		return -1;
	}

	memset(&request, 0, sizeof(request));
	strlcpy(request.ifra_name, INTERFACE, IF_NAMESIZE);
	if (set_address(&request.ifra_addr, LOCAL_ADDRESS) != 0
		|| set_address(&request.ifra_mask, NETMASK) != 0
		|| set_address(&request.ifra_broadaddr, BROADCAST) != 0) {
		say("hellonet: could not parse the built-in addresses");
		return -1;
	}

	say("hellonet: addressing it %s/%s", LOCAL_ADDRESS, NETMASK);
	if (ioctl(socketDescriptor, SIOCAIFADDR, &request, sizeof(request)) < 0) {
		say("hellonet: address failed: %s", strerror(errno));
		return -1;
	}

	struct ifreq flagsRequest;
	memset(&flagsRequest, 0, sizeof(flagsRequest));
	strlcpy(flagsRequest.ifr_name, INTERFACE, IF_NAMESIZE);

	if (ioctl(socketDescriptor, SIOCGIFFLAGS, &flagsRequest,
			sizeof(flagsRequest)) < 0) {
		say("hellonet: reading flags failed: %s", strerror(errno));
		return -1;
	}

	flagsRequest.ifr_flags |= IFF_UP;
	say("hellonet: bringing it up (flags %#x)", flagsRequest.ifr_flags);
	if (ioctl(socketDescriptor, SIOCSIFFLAGS, &flagsRequest,
			sizeof(flagsRequest)) < 0) {
		say("hellonet: up failed: %s", strerror(errno));
		return -1;
	}

	return 0;
}


/*!	One ICMP echo request, and whatever comes back.

	A raw socket, because that is what an echo request needs and what Haiku's own
	ping uses. The receive has a timeout rather than blocking forever: a machine
	that never answers is the result this is looking for as much as one that
	does, and a test that hangs reports neither.

	The reply arrives with its IP header attached -- that is what a raw socket
	delivers -- so the ICMP message starts one header length in, and that length
	comes from the header rather than being assumed to be twenty.
*/
static int
ping_peer(void)
{
	int raw = _kern_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (raw < 0) {
		say("hellonet: raw ICMP socket failed: %s", strerror(raw));
		return -1;
	}

	struct timeval timeout = { 5, 0 };
	_kern_setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	struct icmp request;
	memset(&request, 0, sizeof(request));
	request.icmp_type = ICMP_ECHO;
	request.icmp_code = 0;
	request.icmp_id = htons(0x4841);		// "HA"
	request.icmp_seq = htons(1);
	request.icmp_cksum = 0;
	request.icmp_cksum = checksum(&request, ICMP_MINLEN);

	struct sockaddr_storage peer;
	if (set_address(&peer, PEER_ADDRESS) != 0) {
		close(raw);
		return -1;
	}

	say("hellonet: echo request to %s", PEER_ADDRESS);
	ssize_t sent = _kern_sendto(raw, &request, ICMP_MINLEN, 0,
		(struct sockaddr*)&peer, sizeof(struct sockaddr_in));
	if (sent < 0) {
		say("hellonet: sendto failed: %s", strerror(sent));
		close(raw);
		return -1;
	}
	say("hellonet: sent %d bytes", (int)sent);

	char buffer[256];
	/*	A real address and a real length, even though neither is wanted: the
		syscall validates both rather than treating a null pair as "do not tell
		me", and returns Invalid Argument before it ever looks at the socket.
		The POSIX wrapper in libnetwork is what normally absorbs that.
	 */
	struct sockaddr_storage from;
	socklen_t fromLength = sizeof(from);
	ssize_t received = _kern_recvfrom(raw, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength);
	if (received < 0) {
		say("hellonet: no reply: %s", strerror(received));
		close(raw);
		return -1;
	}

	say("hellonet: received %d bytes", (int)received);

	struct ip* header = (struct ip*)buffer;
	size_t headerLength = (size_t)header->ip_hl * 4;
	if ((size_t)received < headerLength + ICMP_MINLEN) {
		say("hellonet: reply too short to be an ICMP message");
		close(raw);
		return -1;
	}

	struct icmp* reply = (struct icmp*)(buffer + headerLength);
	say("hellonet: icmp type %d code %d id %#x seq %d", reply->icmp_type,
		reply->icmp_code, ntohs(reply->icmp_id), ntohs(reply->icmp_seq));

	close(raw);
	return reply->icmp_type == ICMP_ECHOREPLY ? 0 : -1;
}


int
main(int argc, char** argv)
{
	say("hellonet: start");

	int socketDescriptor = _kern_socket(AF_INET, SOCK_DGRAM, 0);
	if (socketDescriptor < 0) {
		say("hellonet: socket failed: %s", strerror(socketDescriptor));
		return 1;
	}
	say("hellonet: socket ok");

	if (configure_interface(socketDescriptor) != 0) {
		close(socketDescriptor);
		return 1;
	}
	say("hellonet: interface up");

	close(socketDescriptor);

	if (ping_peer() != 0) {
		say("hellonet: no answer");
		return 1;
	}

	say("hellonet ok");
	return 0;
}
