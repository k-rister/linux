#!/usr/bin/perl
# SPDX-License-Identifier: GPL-2.0-only

use strict;
use warnings;

use IO::Socket::INET;
use Socket qw(MSG_DONTWAIT);

my ($listen_addr, $listen_port) = @ARGV;
die "usage: $0 LISTEN-ADDR LISTEN-PORT\n" unless defined $listen_port;

my $listener = IO::Socket::INET->new(
    LocalAddr => $listen_addr,
    LocalPort => $listen_port,
    Proto => 'tcp',
    Listen => 1,
    ReuseAddr => 1,
) or die "listen failed: $!\n";

my $socket = $listener->accept or die "accept failed: $!\n";

while (1) {
    my $data = '';
    recv($socket, $data, 4096, MSG_DONTWAIT);
    my $received = length($data);
    if ($received) {
        warn "received $received bytes\n";
    }
    select undef, undef, undef, 0.001;
}
