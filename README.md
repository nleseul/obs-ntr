# obs-ntr

## Introduction

The obs-ntr plugin is a plugin for [OBS Studio](https://obsproject.com) which is intended to allow a direct
connection to the [NTR debugger](https://github.com/44670/NTR) running on a Nintendo 3DS for purposes of 
displaying NTR's remote view image stream. EFfectively, this allows you to display your 3DS screen within 
OBS Studio. 

This does require that you have a means of installing homebrew applications on your 3DS, and also only works 
with the "New" 3DS hardware. Explaining how to do this is far beyond the scope of this document; if you do
not already have your 3DS set up to run NTR, please refer to guides like [this](https://3ds.guide/) and 
[this](https://blog.lvlupdojo.com/how-to-stream-from-your-nintendo-3ds-12d7fd115981) which explain the process
far better than I'd be likely to. 

Given a working connection to NTR, this plugin allows you to display the remote view from it without the need
to run NTRViewer or any other external application. It treats the top and bottom screens from the 3DS as
separate sources, meaning they can be positioned and filtered independently if desired. 

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
