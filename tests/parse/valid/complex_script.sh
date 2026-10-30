# POSIX-ish sample covering several constructs
PREFIX=app
run() {
	printf '%s\n' "$1"
}
for item in one two three; do
	if true; then
		run "$PREFIX-$item" || run fallback
	else
		case "$item" in
			one) run first ;;
			*) run other ;;
		esac
	fi
done < /dev/null
