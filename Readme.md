# FBFF

A small ffmpeg-based media player for Linux framebuffer and Nano-X

```
FBFF
====

Fbff is a framebuffer/OSS media player using ffmpeg or libmpeg3.

USAGE
=====

To start it simply run:

  $ fbff file.mp4

When playing video files, audio and video may get out of sync.  So I
suggest using this by default:

  $ fbff -u -s file.mp4

And if that results in choppy playback, use:

  $ fbff -u -s100 file.mp4

This means record A/V diff after the first few video frames (-u)
and synchronize each 100 video frames (-s100).  This should work for
most files.

The following table describes fbff keybinding.  Most of these commands
accept a numerical prefix.  The variable avdiff is used to synchronize
audio and video streams.  The synchronization is done after the 's' key
or the pause and seek commands.  '-', '+', and 'a' keys can be used to
change the value of avdiff as explained below.

==============	================================================
KEY		ACTION
==============	================================================
p/space		pause
q		quit
i		print info
l/j/J		seek forward 10s/60s/600s
h/k/K		seek backward 10s/60s/600s
G		seek to the given minute
%		seek to the specified position in percents
^[/escape	clear numerical prefix
mx		mark position as 'x'
'x		jump to position marked as 'x'
s		synchronize audio/video with A-V equal to avdiff
-		set avdiff to -arg
+		set avdiff to +arg
a		set avdiff to current playback A-V diff
c		set synchronization steps
==============	================================================

OPTIONS AND KEYS
================

The following options can be specified when starting fbff:

==============	================================================
OPTION		DESCRIPTION
==============	================================================
-z x		specify ffmpeg video zoom
-m x		magnify the video by duplicating pixels
-j x		jump every x video frames; for slow machines
-f		start full screen
-v x		select video stream; '-' disables video
-a x		select audio stream; '-' disables audio
-s		don't rely on video frame-rate; always synchronize
-u		record avdiff after the first few frames of video
-t path		the file containing the subtitles
-x x		adjust video position horizontally
-y x		adjust video position vertically
-w x		set video width
-h x		set video height
-r		adjust the video to the right of the screen
-b		adjust the video to the bottom of the screen
==============	================================================
```

![screenshot](/fbff-nano-X.png)

FBFF running on Nano-X
