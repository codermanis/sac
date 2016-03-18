# sac
C/C++ helpers

# sac_tweak.h

Implements C/C++ auto-reload of variables directly from the source code.
Usage:
```c
		TWEAK(float, val) = 1.2f;
		TWEAK(char_ptr, name) = "world";
		printf("Hello %s (%f)\n", name, val);
```
You can then launch your application, and edit variable in your source code and the values will change in your running app.

No need to stop, compile and re-run.

(idea based on https://github.com/SoupeauCaillou/sac/blob/master/util/Tuning.h and cbloom's "03-11-16 | Delight" blog post)
