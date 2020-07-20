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

all: check

CC = g++
COMMON_CPP = multilink.cpp multilink.h
COMMON_DEP = ${COMMON_CPP}

test_build: test.cpp ${COMMON_DEP}
	${CC} -o test -g -w ${INCLUDE} ${COMMON_CPP} test.cpp -lpthread -lisal -std=c++11 -Ofast -march=native
test_run:
	sh run.sh
clean:
	rm test

check: test_build test_run
