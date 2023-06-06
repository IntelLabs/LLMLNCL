Intel(R) Low Latency Multi Link Network Coding Library
=================================================

Building LLMLNCL on Linux
--------------

* Make: GNU 'make'

* Compiler: gcc or clang.

* Library dependencies: pthread and isa-l

  If missing

  sudo apt-get install pthread

  sudo apt-get install libisal2

  sudo apt-get install libisal-dev

If there is no libisal2 for your Linux - download, build and install from:
https://github.com/intel/isa-l.git

* make

Running LLMLNCL test on Linux
--------------

* You'll need root to run LLMLNCL test

* Simply run 'make' or 'make check'


Building LLMLNCL on Windows
--------------

* Compiler: Microsoft or Intel compiler for Windows

* Library dependencies: isa-l

* Download, build and install isa-l into LLMLNCL root folder or some other folder from:
  https://github.com/intel/isa-l.git

* set REPO to the folder with isa-l if it is not LLMLNCL root folder

* compile.bat

Running LLMLNCL test on Windows
--------------

* Make sure isa-l dynamic library is in the library path or root folder

* run.bat

* 3 tets (should run around 1 minute) should print "Test passed"
