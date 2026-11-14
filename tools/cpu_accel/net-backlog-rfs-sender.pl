#!/usr/bin/perl
# SPDX-License-Identifier: GPL-2.0-only

use strict;
use warnings;

use IO::Socket::INET;
use Socket qw(AF_INET IPPROTO_TCP inet_aton sockaddr_in SOL_SOCKET TCP_CORK);

my ($peer_addr, $peer_port, $local_addr, $local_dev) = @ARGV;
die "usage: $0 PEER-ADDR PEER-PORT LOCAL-ADDR LOCAL-DEV\n"
    unless defined $local_dev;

my $socket = IO::Socket::INET->new(
    LocalAddr => $local_addr,
    Domain => AF_INET,
    Proto => 'tcp',
) or die "connect failed: $!\n";
setsockopt($socket, SOL_SOCKET, Socket::SO_BINDTODEVICE(),
           pack('Z*', $local_dev))
    or die "bind to device failed: $!\n";
my $peer_sockaddr = sockaddr_in($peer_port, inet_aton($peer_addr));
connect($socket, $peer_sockaddr) or die "connect failed: $!\n";

print $socket 'p' or die "prime send failed: $!\n";
select undef, undef, undef, 0.05;

setsockopt($socket, IPPROTO_TCP, TCP_CORK, 1)
    or die "set TCP_CORK failed: $!\n";
print $socket 'x' or die "corked send failed: $!\n";

my $uncork = 0;
$SIG{USR1} = sub { $uncork = 1; };

while (1) {
    if ($uncork) {
        setsockopt($socket, IPPROTO_TCP, TCP_CORK, 0)
            or die "clear TCP_CORK failed: $!\n";
        warn "uncorked\n";
        $uncork = 0;
    }
    select undef, undef, undef, 0.01;
}
