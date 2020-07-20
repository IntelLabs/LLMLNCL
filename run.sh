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
#  Written by: Evgeny Stupachenko
#              e-mail: evgeny.v.stupachenko@intel.com
#
###########################################################################

#!bash

list_loc="1"
list_rem="1"
for loc in $list_loc
do
  for rem in $list_rem
  do
    sudo ./test 2 1 1 1 1
    ret=`echo $?`
    if [ "$ret" -eq "0" ] ; then
      echo test handshake on ${loc}x${rem} OK
    else
      echo test handshake on ${loc}x${rem}  FAIL
    fi

    sudo ./test 0 1 1 1 1
    ret=`echo $?`
    if [ "$ret" -eq "0" ] ; then
      echo test 1024 packets with increment on ${loc}x${rem} redunduncy 1,1 OK
    else
      echo test 1024 packets with increment on ${loc}x${rem} redunduncy 1,1 FAIL
    fi

    sudo ./test 1 1 1 1 1
    ret=`echo $?`
    if [ "$ret" -eq "0" ] ; then
      echo test 32768 size packet on ${loc}x${rem} redunduncy 1,1 OK
    else
      echo test 32768 size packet on ${loc}x${rem} redunduncy 1,1 FAIL
    fi
  done
done
