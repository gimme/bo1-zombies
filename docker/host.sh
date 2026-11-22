# docker/host.sh - sourced by the docker/*.sh that run on the host, so they run the same on Linux and in Git Bash on
# Windows (Docker Desktop).
#
# abs <dir>   the directory's absolute path, as docker takes a host path (for -v, a build's context): in Git Bash,
#             C:/..., since docker.exe is a Windows program and Git Bash's own paths (/c/..., /tmp) mean nothing to it
# uid, gid    who the containers run as, and the images' user: the user's own on Linux, so they own what the
#             containers write; in Git Bash 1000:1000, the images' default, since Windows owns every file there anyway
#
# Git Bash would also rewrite docker's arguments that look like paths as Windows ones: the container's (/src), and a
# volume's host:container pair as a list of two (C:\...;C:\Program Files\Git\...). MSYS_NO_PATHCONV=1 stops it.
if [ -n "${MSYSTEM:-}" ]; then
    export MSYS_NO_PATHCONV=1
    abs() { (cd "${1:?}" && pwd -W); }
    uid=1000 gid=1000
else
    abs() { (cd "${1:?}" && pwd); }
    uid=$(id -u) gid=$(id -g)
fi
