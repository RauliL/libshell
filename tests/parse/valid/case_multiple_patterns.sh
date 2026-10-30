case x in
	a|b|c) echo letter ;;
	[0-9]) echo digit ;;
	*) echo other ;;
esac
