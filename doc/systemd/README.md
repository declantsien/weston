systemd integration examples
============================

These examples rely on Weston's logind and systemd support. Weston needs to be
built with options: `--enable-dbus --enable-systemd-login
--enable-systemd-notify`

Furthermore, Weston needs to be configured to load systemd-notify.so plugin.
This can be done on the Weston command line:

	$ weston --modules=systemd-notify.so

or in weston.ini:

~~~
[core]
modules=systemd-notify.so
~~~

The plugin implements the systemd service notification protocol, watchdog
protocol, and also allows socket activation and configuring listening sockets
via systemd.


weston@.service
---------------

`weston@.service` is an example systemd service file on how to run Weston as a
system service. The service starts a user session of the named user on the given
virtual terminal. This is useful for running a login manager or for dedicated
systems that do not have personal user accounts and do not need the user to log
in.

The service uses the PAM service name `weston-autologin` to setup the user
session. Make sure to install a suitable PAM configuration file in
`/etc/pam.d/weston-autologin`. A basic configuration file could look like this:

~~~
auth      required  pam_nologin.so
auth      required  pam_unix.so     try_first_pass nullok

account   required  pam_nologin.so
account   required  pam_unix.so

session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_systemd.so type=wayland class=user desktop=weston
-session  optional  pam_loginuid.so
~~~

Install the service template to `/etc/systemd/system`. You should at the very
least customize the user, but likely also the weston command line. You can edit
the service template before installing, or use `systemctl edit` if the service
template is already installed. The tty can be customized by the instance name
(the portion after the @ symbol). To enable and start the service use:

	systemctl enable weston@tty7.service
	systemctl start weston@tty7.service

Note: With this unit pam_systemd creates a new user session and session scope
with it. With that systemd now longer considers Weston to be part of the
weston@ttyX.service unit, but the newly created session slice. Use `loginctl` to
find the session name. Logs can be found in `journalctl
--unit=session-<name>.scope`.

weston@.socket
--------------

`weston@.socket` shows how to start Weston on-demand using systemd's socket
activation for the Wayland UNIX domain stream socket. The example creates a UNIX
domain socket with the name `wayland-<instance-name>`, hence clients need to use
this name as `WAYLAND_DISPLAY`.

Install the socket template to `/etc/systemd/system`. Make sure the user id
matches your setup. To enable the socket use:

	systemctl enable weston@tty7.socket
	systemctl start weston@tty7.socket

With that, starting a Wayland client with the environment set to
`WAYLAND_DISPLAY=wayland-tty7` will start the `weston@tty7.service` and display
the client once started.
