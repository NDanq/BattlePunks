@echo off
rem Stands in for ffmpeg under FrameRecorder (capture.ps1): the last raw frame as <out>.bmp.
py -3 "%~dp0dumpframe.py" %*
