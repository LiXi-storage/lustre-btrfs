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
		AS_IF([test x$enable_btrfs = xmaybe], [enable_btrfs=yes])

		AC_DEFINE(HAVE_BTRFS_OSD, 1, Enable btrfs osd)
	])

	AC_MSG_CHECKING([whether to build btrfs])
	AC_MSG_RESULT([$enable_btrfs])

	AM_CONDITIONAL([BTRFS_ENABLED], [test x$enable_btrfs = xyes])
])
