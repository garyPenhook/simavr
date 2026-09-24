/*
	fuzz_main.c — real main() for Csmith programs built by tests/fuzz.

	runtime/csmith.h renames the generated main() to csmith_main(). The AVR
	C runtime calls main() with undefined argc/argv, so pass argc == 1 here,
	which keeps print_hash_value = 0 and never touches argv[1].
 */
int csmith_main(int argc, char *argv[]);

int main(void)
{
	static char name[] = "csmith";
	char *argv[] = { name, 0 };
	return csmith_main(1, argv);
}
