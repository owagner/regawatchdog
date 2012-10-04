CC=../hmtc/toolchain_build_arm/gcc-3.3.6-final/gcc/gcc-cross

regawatchdog: regawatchdog.c
	$(CC) -Wall -o regawatchdog regawatchdog.c
	
