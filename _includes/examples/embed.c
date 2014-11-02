#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <ruby.h>

/* constants */
const unsigned int win_width = 1024;
const unsigned int win_height = 768;
const unsigned int actor_size = 30;
const char* ai_script = "ai.rb";

/* globals */
bool ai_loaded = false;
bool ai_error = false;
time_t ai_load_time;

/* for position and direction */
struct vec2
{
	float x;
	float y;
};

/* for the player and their opponent */
struct actor
{
	struct vec2 pos;
	struct vec2 dir;
	float speed;
	SDL_Color color;
};

/* for handling exceptions from protect calls */
void handle_ruby_error()
{
	ai_error = true;

	VALUE exception = rb_errinfo();
	rb_set_errinfo(Qnil);

	rb_warn("AI script error: %"PRIsVALUE"\n", exception);
}

/* try to (re)load AI script */
void update_ai(struct actor* act)
{
	/* open script */
	FILE* script = fopen(ai_script, "rb");
	if (!script)
	{
		if (ai_loaded)
			fprintf(stderr, "Can't load AI script\n");
		ai_loaded = false;
		return;
	}

	/* get script modification time */
	struct stat script_stat;
	if (fstat(fileno(script), &script_stat))
	{
		if (ai_loaded)
			fprintf(stderr, "Can't stat AI script\n");
		ai_loaded = false;
		fclose(script);
		return;
	}

	/* return if we've already loaded the script and it hasn't been updated */
	if (ai_loaded)
	{
		if (ai_load_time >= script_stat.st_mtime)
		{
			fclose(script);
			return;
		}
	}
	else
		ai_loaded = true;

	ai_load_time = script_stat.st_mtime;

	fprintf(stderr, "Loading AI script...\n");

	/* TODO there's probably a more proper way to load a script */

	/* read script */
	char* buffer = malloc(script_stat.st_size + 1);
	if (!buffer)
	{
		fprintf(stderr, "malloc failure\n");
		exit(1);
	}
	fread(buffer, 1, script_stat.st_size, script);
	buffer[script_stat.st_size] = '\0';
	fclose(script);

	/* reset error state */
	ai_error = false;
	act->color.a = 255;

	/* run script */
	int state;
	rb_eval_string_protect(buffer, &state);

	free(buffer);

	if (state)
		handle_ruby_error();
}

/* for rescuing exceptions in the AI script */
VALUE think_wrapper(VALUE actors)
{
	rb_funcall(rb_mKernel, rb_intern("think"), 2, rb_ary_entry(actors, 0), rb_ary_entry(actors, 1));

	return Qundef;
}

/* run the AI script if possible */
void ai_think(struct actor* act, VALUE ai_v, VALUE player_v)
{
	/* indicate that AI isn't running */
	if (!ai_loaded || ai_error)
	{
		act->dir.x = 0.f;
		act->dir.y = 0.f;
		act->color.a = 127;
		return;
	}

	int state;
	rb_protect(think_wrapper, rb_ary_new_from_args(2, ai_v, player_v), &state);

	if (state)
		handle_ruby_error();
}

/* move actor after ms time has elapsed */
void step_actor(struct actor* act, unsigned int ms)
{
	float norm = sqrtf(act->dir.x * act->dir.x + act->dir.y * act->dir.y);
	if (norm == 0.f)
		return;
	/* clamp magnitude to 1 */
	else if (norm > 1.f)
		norm = 1.f / norm;
	else
		norm = 1.f;

	act->pos.x += act->dir.x * act->speed * (float)ms * norm;
	act->pos.y += act->dir.y * act->speed * (float)ms * norm;

	// clamp to screen
	if (act->pos.x < 0.f)
		act->pos.x = 0.f;
	else if (act->pos.x > win_width - actor_size)
		act->pos.x = win_width - actor_size;
	if (act->pos.y < 0.f)
		act->pos.y = 0.f;
	else if (act->pos.y > win_height - actor_size)
		act->pos.y = win_height - actor_size;
}

/* draw an actor as a colored box */
void draw_actor(SDL_Renderer* renderer, struct actor* act)
{
	SDL_SetRenderDrawColor(renderer, act->color.r, act->color.g, act->color.b, act->color.a);
	SDL_Rect rectangle = { .x = act->pos.x, .y = act->pos.y, .w = actor_size, .h = actor_size };
	SDL_RenderFillRect(renderer, &rectangle);
}

/* methods for the API we're defining for the AI script */
/* time - returns total elapsed time in milliseconds */
VALUE m_time(VALUE self)
{
	return UINT2NUM(SDL_GetTicks());
}

/* Actor#pos - returns screen position x, y in pixels */
VALUE actor_m_pos(VALUE self)
{
	struct actor* data;
	Data_Get_Struct(self, struct actor, data);

	return rb_ary_new_from_args(2, DBL2NUM(data->pos.x), DBL2NUM(data->pos.y));
}

/* Actor#pos - returns last movement direction x, y. each is in the range (-1..1) */
VALUE actor_m_dir(VALUE self)
{
	struct actor* data;
	Data_Get_Struct(self, struct actor, data);

	return rb_ary_new_from_args(2, DBL2NUM(data->dir.x), DBL2NUM(data->dir.y));
}

/* Actor#move - set next movement direction. x, y as in Actor#pos */
VALUE actor_m_move(VALUE self, VALUE x, VALUE y)
{
	Check_Type(x, T_FLOAT);
	Check_Type(y, T_FLOAT);

	float nx = NUM2DBL(x);
	float ny = NUM2DBL(y);

	struct actor* data;
	Data_Get_Struct(self, struct actor, data);

	data->dir.x = nx;
	data->dir.y = ny;

	return Qnil;
}

int main(int argc, char** argv)
{
	/* start Ruby TODO is this redundant? */
	if (ruby_setup())
	{
		fprintf(stderr, "Failed to init Ruby VM\n");
		return 1;
	}
	/* set a nicer script name than <main> */
	ruby_script("ruby");

	/* define our own little API for use in the AI script */
	rb_define_global_function("time", m_time, 0);

	/* Actor will wrap struct actor for passing to Ruby */
	VALUE cActor = rb_define_class("Actor", rb_cObject);
	rb_define_method(cActor, "pos", actor_m_pos, 0);
	rb_define_method(cActor, "dir", actor_m_dir, 0);
	rb_define_method(cActor, "move", actor_m_move, 2);

	/*
	 * Notice that even though Actor wraps C data, we didn't define an
	 * allocation or free function. That's because we're going to create all
	 * the actors in C and expose them to Ruby. However we should make sure
	 * that Ruby can't create new Actors, because they'll contain invalid data
	 * pointers
	 */
	rb_undef_method(rb_singleton_class(cActor), "new");

	/* start SDL */
	SDL_Init(SDL_INIT_VIDEO);

	/* create window */
	SDL_Window* window = SDL_CreateWindow(
		"Tag",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		win_width,
		win_height,
		0
	);
	if (window == NULL)
	{
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 1;
	}

	/* create renderer */
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (renderer == NULL)
	{
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

	/* create actors */
	struct actor player = {
		.pos = { .x = win_width / 2.f + 100.f - actor_size / 2.f, .y = win_height / 2.f - actor_size / 2.f },
		.dir = { .x = 0.f, .y = 0.f },
		.speed = 0.5f,
		.color = { .r = 0, .g = 0, .b = 255, .a = 255 }
	};
	struct actor ai = {
		.pos = { .x = win_width / 2.f - 100.f - actor_size / 2.f, .y = win_height / 2.f - actor_size / 2.f },
		.dir = { .x = 0.f, .y = 0.f },
		.speed = 0.55f,
		.color = { .r = 255, .g = 0, .b = 0, .a = 255 }
	};

	/* create Ruby objects for actors */
	VALUE player_v = Data_Wrap_Struct(cActor, NULL, NULL, &player);
	VALUE ai_v = Data_Wrap_Struct(cActor, NULL, NULL, &ai);

	/* don't allow the player to be moved via the AI script */
	rb_undef_method(rb_singleton_class(player_v), "move");

	/* set up timing */
	unsigned int ai_step = 33; /* run AI at 30fps */
	unsigned int last_time = SDL_GetTicks();
	unsigned int now;
	unsigned int frame_time;
	unsigned int ai_time;

	/* start up AI */
	update_ai(&ai);
	ai_think(&ai, ai_v, player_v);

	/* for player input */
	const Uint8* keyboard = SDL_GetKeyboardState(NULL);

	/* main loop */
	SDL_Event event;
	bool running = true;
	while (running)
	{
		/* update timers */
		now = SDL_GetTicks();
		frame_time = now - last_time;
		ai_time += frame_time;
		last_time = now;

		/* event handling */
		while (SDL_PollEvent(&event))
		{
			switch(event.type)
			{
				case SDL_QUIT:
					running = false;
					break;
				case SDL_KEYDOWN:
					if (event.key.keysym.sym == SDLK_ESCAPE)
					{
						running = false;
						break;
					}
			}
		}

		/* player movement */
		player.dir.x = 0.f;
		player.dir.y = 0.f;

		if (keyboard[SDL_SCANCODE_UP])
			player.dir.y -= 1.f;
		if (keyboard[SDL_SCANCODE_DOWN])
			player.dir.y += 1.f;
		if (keyboard[SDL_SCANCODE_LEFT])
			player.dir.x -= 1.f;
		if (keyboard[SDL_SCANCODE_RIGHT])
			player.dir.x += 1.f;

		/* AI movement */
		if (ai_time >= ai_step)
		{
			update_ai(&ai);
			ai_think(&ai, ai_v, player_v);

			ai_time %= ai_step;
		}

		/* game step */
		step_actor(&ai, frame_time);
		step_actor(&player, frame_time);

		/* render */
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
		SDL_RenderClear(renderer);

		draw_actor(renderer, &ai);
		draw_actor(renderer, &player);

		SDL_RenderPresent(renderer);

		/* let CPU rest */
		SDL_Delay(1);
	}

	/* clean up */
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	/* stop SDL */
	SDL_Quit();

	/* stop Ruby */
	return ruby_cleanup(0);
}
