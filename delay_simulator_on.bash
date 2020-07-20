###########################################################################
#  Copyright 2020 Intel Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
###########################################################################
#
# Written by: Andrey Belogolovy
#             e-mail: andrey.belogolovy@intel.com
###########################################################################

# use lo as an interface for all packets
#
# simplest: one for all
#tc qdisc add dev lo root handle 1: netem delay 15ms 5ms distribution normal loss 0.5% reorder 25% 50% rate 8mbit
#
#
# second simplest: one for all, but rate limit as a separate tool
#tc qdisc add dev lo root handle 1: netem delay 15ms 5ms distribution normal loss 0.5% reorder 25% 50%
#tc qdisc add dev lo parent 1: handle 2: tbf rate 8mbit burst 64kb latency 200ms
#
#
tc qdisc add dev lo handle 1: root htb r2q 1700
tc class add dev lo parent 1: classid 1:1 htb rate 100Mbps ceil 100Mbps
tc class add dev lo parent 1:1 classid 1:11 htb rate 100Mbps
tc class add dev lo parent 1:1 classid 1:12 htb rate 100Mbps
tc class add dev lo parent 1:1 classid 1:13 htb rate 100Mbps
tc class add dev lo parent 1:1 classid 1:14 htb rate 100Mbps
tc class add dev lo parent 1:1 classid 1:15 htb rate 100Mbps
tc class add dev lo parent 1:1 classid 1:16 htb rate 100Mbps
# channel 1, source and destimation port = 3000, i.e. both ways
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip sport 3000 0xFFFF flowid 1:11
tc qdisc add dev lo parent 1:11 handle 20: netem delay 10ms 2ms distribution normal loss 0.1% reorder 25% 50% rate 8mbit
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip dport 3000 0xFFFF flowid 1:12
tc qdisc add dev lo parent 1:12 handle 30: netem delay 10ms 2ms distribution normal loss 0.1% reorder 25% 50% rate 10mbit
# channel 2, source and destination port = 3001, i.e. both ways
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip sport 3001 0xFFFF flowid 1:13
tc qdisc add dev lo parent 1:13 handle 40: netem delay 15ms 5ms distribution normal loss 0.5% reorder 25% 50% rate 4mbit
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip dport 3001 0xFFFF flowid 1:14
tc qdisc add dev lo parent 1:14 handle 50: netem delay 15ms 5ms distribution normal loss 0.5% reorder 25% 50% rate 5mbit
# channel 3, source and destination port = 3002, i.e. both ways
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip sport 3002 0xFFFF flowid 1:15
tc qdisc add dev lo parent 1:15 handle 60: netem delay 15ms 5ms distribution normal loss 0.5% reorder 25% 50% rate 3mbit
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip dport 3002 0xFFFF flowid 1:16
tc qdisc add dev lo parent 1:16 handle 70: netem delay 10ms 5ms distribution normal loss 0.5% reorder 25% 50% rate 6mbit
# display rules
tc qdisc show dev lo
