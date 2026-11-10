int main(int, char **, char **);
int __getmainargs(int *, char ***, char ***, int, int *);

__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);

void mainCRTStartup()
{
    int    argc;
    char **argv;
    char **envp;
    int    s;
    if (__getmainargs(&argc, &argv, &envp, 0, &s) < 0)
        ExitProcess(-1);
    else
        ExitProcess(main(argc, argv, envp));
}

long _fltused = 1;
