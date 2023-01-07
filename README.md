# QFileTransport Plugin for Audacious

This is a transport plugin for the [Audacious audio player](https://audacious-media-player.org/). This plugin uses the Qt file I/O subsystem for local files, which supports more edge cases. For example, with this plugin you can play audio files with paths like `file://hostname/share/path/to/file.mp3` on Windows from your SMB server.

The implementation of Audacious's VFS does not allow override the transport for local files (see [libaudcore/vfs.cc](https://github.com/audacious-media-player/audacious/blob/audacious-4.2/src/libaudcore/vfs.cc#L45-L46) for details). For this reason, the plugin is not cross-platform and uses a platform-specific hook, which is implemented only for 32-bit Windows builds at this moment.
