libcds implements low-level succinct data structures such as bitmaps,
sequences, permutations, etc. The main goal is to provide a set of
structures that form the building block of most compressed/succinct
solutions. In the near future we are planning to add compression
algorithms and support for succinct trees.

You can find more information at http://libcds.recoded.cl/

Right now we are just fixing bugs and testing some new things in here,
but most of the effort is being put into libcds2, this rewrite aims at
solving many problems with 64bits systems. This is necessary in order
to index bigger inputs.

BUILD INSTRUCTIONS
==================

To do a normal build for your platform, just do:

    ./configure && make && make check

To install "make install".  If you want the library installed elsewhere,
run the configure script at the beginning with a --prefix flag
("./configure --prefix=/usr/local").  Run ./configure --help for more
information on the build configuration script and refer to the INSTALL
file for more information.

BUILDING DOCUMENTATION
======================

Documentation is provided by doxygen. This can be built by running:

    make doxygen-doc

You'll need the doxygen tools to build these docs however. On an Ubuntu
machine do the following to install all the required packages.

    sudo apt-get install doxygen texlive-fonts-recommended texlive-fonts-extra

CROSS BUILD INSTRUCTIONS
========================
To build for a windows target on an Ubuntu system you can use the mingw32
cross compiler:

    sudo apt-get install g++-mingw-w64
    ./configure --host=i686-w64-mingw32
    make
