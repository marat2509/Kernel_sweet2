#
# Makefile for the FUSE filesystem.
#

ccflags-y += -O3

obj-$(CONFIG_FUSE_FS) += fuse.o
obj-$(CONFIG_CUSE) += cuse.o

fuse-objs := dev.o dir.o file.o inode.o control.o xattr.o acl.o
fuse-objs += passthrough.o
