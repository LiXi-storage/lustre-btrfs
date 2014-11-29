#
# BTRFS_LINUX_SERIES
#
AC_DEFUN([BTRFS_LINUX_SERIES], [
BTRFS_SERIES=
AC_MSG_CHECKING([which btrfs series to use])
AS_IF([test x$RHEL_KERNEL = xyes], [
	case $RHEL_RELEASE_NO in
	71)	BTRFS_SERIES="3.10-rhel7.series"	;;
	esac
])
AS_IF([test -z "$BTRFS_SERIES"],
	[AC_MSG_WARN([Unknown kernel version $BTRFS_VERSIONRELEASE])])
AC_MSG_RESULT([$BTRFS_SERIES])
AC_SUBST(BTRFS_SERIES)
]) # BTRFS_LINUX_SERIES

#
# LB_VALIDATE_BTRFS_SRC_DIR
#
# Spot check the existance of several source files common to btrfs.
# Detecting this at configure time allows us to avoid a potential build
# failure and provide a useful error message to explain what is wrong.
#
AC_DEFUN([LB_VALIDATE_BTRFS_SRC_DIR], [
enable_btrfs_build="no"
AS_IF([test -n "$BTRFS_SRC_DIR"], [
	enable_btrfs_build="yes"
	LB_CHECK_FILE([$BTRFS_SRC_DIR/ctree.c], [], [
		enable_btrfs_build="no"
		AC_MSG_WARN([ctree.c must exist for btrfs build])
	])
	LB_CHECK_FILE([$BTRFS_SRC_DIR/file.c], [], [
		enable_btrfs_build="no"
		AC_MSG_WARN([file.c must exist for btrfs build])
	])
	LB_CHECK_FILE([$BTRFS_SRC_DIR/inode.c], [], [
		enable_btrfs_build="no"
		AC_MSG_WARN([inode.c must exist for btrfs build])
	])
	LB_CHECK_FILE([$BTRFS_SRC_DIR/super.c], [], [
		enable_btrfs_build="no"
		AC_MSG_WARN([super.c must exist for btrfs build])
	])
])

AS_IF([test "x$enable_btrfs_build" = xno], [
	enable_btrfs="no"

	AC_MSG_WARN([

Disabling btrfs support because complete btrfs source does not exist.

If you are building using kernel-devel packages and require btrfs
server support then ensure that the matching kernel-debuginfo-common
and kernel-debuginfo-common-<arch> packages are installed.
])
])
]) # LB_VALIDATE_BTRFS_SRC_DIR

#
# LB_BTRFS_SRC_DIR
#
# Determine the location of the btrfs source code.  It it required
# for several configure tests and to build btrfs.
#
AC_DEFUN([LB_BTRFS_SRC_DIR], [
AC_MSG_CHECKING([btrfs source directory])
# Kernel ext source located with devel headers
linux_src=$LINUX
AS_IF([test -e "$linux_src/fs/btrfs/super.c"], [
	BTRFS_SRC_DIR="$linux_src/fs/btrfs"
], [
	# Kernel ext source provided by kernel-debuginfo-common package
	linux_src=$(ls -1d /usr/src/debug/*/linux-$LINUXRELEASE \
		2>/dev/null | tail -1)
	AS_IF([test -e "$linux_src/fs/btrfs/super.c"],
		[BTRFS_SRC_DIR="$linux_src/fs/btrfs"],
		[BTRFS_SRC_DIR=""])
])
AC_MSG_RESULT([$BTRFS_SRC_DIR])
AC_SUBST(BTRFS_SRC_DIR)

LB_VALIDATE_BTRFS_SRC_DIR
]) # LB_BTRFS_SRC_DIR

AC_DEFUN([LB_CONFIG_BTRFS], [

	AC_ARG_ENABLE([btrfs],
		[AS_HELP_STRING([--disable-btrfs],
			[disable btrfs osd (default is enable)])],
		[AS_IF([test x$enable_btrfs != xyes -a x$enable_btrfs != xno],
			[AC_MSG_ERROR([btrfs valid options are "yes" or "no"])])],
		[AS_IF([test "${with_btrfs+set}" = set],
			[enable_btrfs=$with_btrfs],
			[enable_btrfs=maybe])
	])

	AS_IF([test x$enable_server = xno],
		[AS_CASE([$enable_btrfs],
			[maybe], [enable_btrfs=no],
			[yes], [AC_MSG_ERROR([cannot build btrfs when servers are disabled])]
		)])

	AS_IF([test x$enable_btrfs != xno],[
		BTRFS_LINUX_SERIES

		AS_IF([test x$enable_btrfs = xmaybe], [enable_btrfs=yes])

		AC_SUBST(BTRFS_SUBDIR, btrfs)
		AC_DEFINE(HAVE_BTRFS_OSD, 1, Enable btrfs osd)
	])

	AC_MSG_CHECKING([whether to build btrfs])
	AC_MSG_RESULT([$enable_btrfs])

	AM_CONDITIONAL([BTRFS_ENABLED], [test x$enable_btrfs = xyes])
])
