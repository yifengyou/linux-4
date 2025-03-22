# linux 4.15.18-kdev

# debug strings

```c

	pr_kdev("### %s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("### %s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("# %s File:[%s],Line:[%d] COMMENT\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("# %s File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("# %s File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);


	pr_kdev("# %s,File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("# %s,File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("## %s,File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("## %s,File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("### %s,File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("### %s,File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("#### %s,File:[%s],Line:[%d] start\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("#### %s,File:[%s],Line:[%d] finished\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("# Func:[%s],File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("## Func:[%s],File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("### Func:[%s],File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);

	pr_kdev("# %s,File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("## %s,File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("### %s,File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);
	pr_kdev("#### %s,File:[%s],Line:[%d]\n", __FUNCTION__, __FILE__, __LINE__);

```

# debug stack

```
	dump_stack();
```




