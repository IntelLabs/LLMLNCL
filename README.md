Intel(R) Low Latency Multi Link Network Coding Library
=================================================

Building LLMLNCL
--------------

* Make: GNU 'make'

* Compiler: gcc or clang.

* Library dependencies: pthread and isa-l

  If missing

  sudo apt-get install pthread

  sudo apt-get install libisal2

  sudo apt-get install libisal-dev

If there is no libisal2 for your Linux - download, build and instal from:
https://github.com/intel/isa-l.git

Running LLMLNCL test
--------------

* You'll need root to run LLMLNCL test

* Simply run 'make' or 'make check'
