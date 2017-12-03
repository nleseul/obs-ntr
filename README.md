# obs-ntr

## Introduction

(TODO)

## Installation

The binary package mirrors the structure of the OBS Studio installation directory, so you should be able to
just drop its contents alongside an OBS Studio install (usually at C:\Program Files (x86)\obs-studio\). The 
necessary files should look like this: 

    obs-studio
    |---data
    |   |---obs-plugins
    |       |---obs-ntr
    |           |---locale
    |               |---en-US.ini
    |---obs-plugins
        |---32bit
        |   |---obs-ntr.dll
        |   |---turbojpeg.dll (????)
        |---64bit
            |---obs-ntr.dll
            |---turbojpeg.dll (????)

## Usage

(TODO)

## Building

(TODO)

## License

This project is licensed under the "[Unlicense](http://unlicense.org/)", because copy[right|left] is a hideous
mess to deal with and I don't like it. 

Unfortunately, this software is based in part on the work of the Independent JPEG Group. I'd love to avoid that
dependency and use the JPEG handling that OBS Studio already provides, but it doesn't seem to be exposed in precisely 
the right way for now. Until then, refer to either the [libjpeg-turbo repository](https://github.com/libjpeg-turbo) or 
the included LICENSE-TurbonJPEG.md file for further details. 
