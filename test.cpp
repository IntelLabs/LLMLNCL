/*
 * Copyright 2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Written by: Evgeny Stupachenko
 *             e-mail: evgeny.v.stupachenko@intel.com
 */

#include "multilink.h"
#include <stdio.h>
#include <unistd.h>

MultiLink MlSend;
MultiLink MlReceive;
uint16_t LocalPort = 3000;
uint16_t RemotePort = 4000;
uint16_t LocalNum = 0;
uint16_t RemoteNum = 0;
uint16_t N = 0;
uint16_t K = 0;
uint16_t TestType = 0;

void *runReceive(void *arg) {
  uint16_t Size = 0;
  uint8_t Data[0xFFFF];

  // For receive Local and Remote are swaped
  MlReceive.setCommDeviceNumber(RemoteNum, LocalNum);
  for (int32_t i = 0; i < RemoteNum; i++)
    MlReceive.addLocalIfaceAndPort("lo", LocalPort + i);
  for (int32_t i = 0; i < LocalNum; i++)
    MlReceive.addRemoteAddrAndPort("127.0.0.1", RemotePort + i);
  MlReceive.setRedundancy(N, K);
  MlReceive.initiateLinks();
  sleep(1);
  if (TestType == 0) {
    // Receiving increment
    for (int32_t i = 0; i < 1024; i++) {
      MlReceive.receive(&Size, Data);
      if (Size != 4) {
        printf("Wrong size on receive %u, expected 4\n", Size);
        exit(-2);  // wrong size on increment receive
      }
      if (*(int32_t *)Data != i) {
        printf("Wrong data on receive %d, expected %d\n", *(int32_t *)Data, i);
        exit(-3);  // wrong increment received
      }
      sleep(0.01);
    }
  } else if (TestType == 1) {
    // Receiving big packet
    sleep(1);
    MlReceive.receive(&Size, Data);
    if (Size != 32768) {
      printf("Wrong size on receive %u, expected 4\n", Size);
      exit(-2);  // wrong size on increment receive
    }
    for (int32_t i = 0; i < 32768 / 4; i++)
      if (((int32_t *)Data)[i] != i) {
        printf("Wrong data on receive %d, expected %d\n", ((int32_t *)Data)[i], i);
        exit(-3);  // wrong increment received
      }
  } else {
    // just handshake
  }
  return NULL;
}

int main(int argc, char *argv[]) {

  uint16_t Cntr = 0, Size;
  uint8_t Data[0xFFFF];
  pthread_t ReceiveThread;

  if (argc < 6)
    return 1;

  TestType = atoi(argv[1]);
  LocalNum = atoi(argv[2]);
  RemoteNum = atoi(argv[3]);
  N = atoi(argv[4]);
  K = atoi(argv[5]);


  MlSend.setCommDeviceNumber(LocalNum, RemoteNum);
  for (int32_t i = 0; i < LocalNum; i++)
    MlSend.addLocalIfaceAndPort("lo", RemotePort + i);
  MlSend.setRedundancy(N, K);
  pthread_create(&ReceiveThread, NULL, runReceive, NULL);
  sleep(2);
  if (TestType == 0) {
    // Sending increment
    for (int32_t i = 0; i < 1024; i++) {
      Size = 4;
      *(int32_t *)Data = i;
      MlSend.send(Size, Data);
      sleep(0.01);
    }
  } else if (TestType == 1) {
    // sending big packet
    for (int32_t i = 0; i < 32768 / 4; i++)
      ((int32_t *)Data)[i] = i;
    Size = 32768;
    MlSend.send(Size, Data);
  } else {
    // just handshake
  }
  sleep(2);
  // if receive thread not finished return error
  if (pthread_cancel(ReceiveThread) == 0)
    return 1;
  return 0;
}
