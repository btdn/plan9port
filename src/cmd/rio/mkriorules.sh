if [ "x$WSYSTYPE" = xnowsys ]; then
	echo 'all install clean nuke:Q:'
	echo '	#'
	exit 0
fi
cat $PLAN9/src/mkmany
