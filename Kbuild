# Out-of-tree build for mdraid personality modules
# Note: md-mod (md.o) is built into vmlinux on RHEL 10 (CONFIG_BLK_DEV_MD=y)
# Focus on raid456 as the primary development target.

raid456-y	+= md/raid5.o md/raid5-cache.o md/raid5-ppl.o

obj-m		+= md/raid0.o
obj-m		+= raid456.o
obj-m		+= isa-l/
