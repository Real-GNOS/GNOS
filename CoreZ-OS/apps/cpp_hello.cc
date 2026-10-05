#include <new>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_ctor_ran = 0;
struct EarlyInit {
    EarlyInit() { g_ctor_ran = 1; }
} g_early;

struct Widget {
    int v;
    Widget(int x) : v(x) {}
};

static void w(const char *s) { write(1, s, strlen(s)); }

int main() {
    Widget *a = new Widget(40);
    Widget *b = new Widget(2);
    int sum = a->v + b->v;
    delete a;
    delete b;

    std::string msg = "cpp_hello";

    bool caught = false;
    try {
        throw 7;
    } catch (int e) {
        caught = (e == 7);
    }

    bool dtor_ok = false;
    {
        struct Guard {
            bool *f;
            ~Guard() { *f = true; }
        } guard{&dtor_ok};
        try {
            throw 1;
        } catch (...) {
        }
    }

    bool ok = (sum == 42) && g_ctor_ran && (msg.size() == 9) && caught && dtor_ok;
    w(ok ? "cpp: PASS\n" : "cpp: FAIL\n");
    _exit(ok ? 0 : 1);
}
