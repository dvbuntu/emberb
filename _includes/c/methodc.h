/* normal mandatory args (can have up to 16 args not counting self) */
VALUE my_method(VALUE self, VALUE arg1, VALUE arg2)
{
	/* ... */
}

/* or, slurp all args into a Ruby Array */
VALUE my_method(VALUE self, VALUE args)
{
	/* ... */
}

/* or, pass all args as a C array */
VALUE my_method(int argc, VALUE* argv, VALUE self)
{
	/* ... */
}
