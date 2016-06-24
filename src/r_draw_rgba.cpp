// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		True color span/column drawing functions.
//
//-----------------------------------------------------------------------------

#include <stddef.h>

#include "templates.h"
#include "doomdef.h"
#include "i_system.h"
#include "w_wad.h"
#include "r_local.h"
#include "v_video.h"
#include "doomstat.h"
#include "st_stuff.h"
#include "g_game.h"
#include "g_level.h"
#include "r_data/r_translate.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "r_plane.h"
#include "r_draw_rgba.h"

#include "gi.h"
#include "stats.h"
#include "x86.h"
#ifndef NO_SSE
#include <emmintrin.h>
#include <immintrin.h>
#endif
#include <vector>

extern int vlinebits;
extern int mvlinebits;
extern int tmvlinebits;
extern uint32_t vlinemax;
extern uint32_t mvlinemax;
extern uint32_t tmvlinemax;

extern "C" short spanend[MAXHEIGHT];
extern float rw_light;
extern float rw_lightstep;
extern int wallshade;

CVAR(Bool, r_multithreaded, true, 0)
CVAR(Bool, r_bilinear, true, 0)
CVAR(Bool, r_mipmap, true, 0)

#ifndef NO_SSE

// Generate SSE drawers:
#define VecCommand(name) name##_SSE_Command
#define VEC_SHADE_SIMPLE_INIT SSE_SHADE_SIMPLE_INIT
#define VEC_SHADE_SIMPLE_INIT4 SSE_SHADE_SIMPLE_INIT4
#define VEC_SHADE_SIMPLE SSE_SHADE_SIMPLE
#define VEC_SHADE_INIT SSE_SHADE_INIT
#define VEC_SHADE_INIT4 SSE_SHADE_INIT4
#define VEC_SHADE SSE_SHADE
#include "r_draw_rgba_sse.h"
/*
// Generate AVX drawers:
#undef VecCommand
#undef VEC_SHADE_SIMPLE_INIT
#undef VEC_SHADE_SIMPLE_INIT4
#undef VEC_SHADE_SIMPLE
#undef VEC_SHADE_INIT
#undef VEC_SHADE_INIT4
#undef VEC_SHADE
#define VecCommand(name) name##_AVX_Command
#define VEC_SHADE_SIMPLE_INIT AVX_LINEAR_SHADE_SIMPLE_INIT
#define VEC_SHADE_SIMPLE_INIT4 AVX_LINEAR_SHADE_SIMPLE_INIT4
#define VEC_SHADE_SIMPLE AVX_LINEAR_SHADE_SIMPLE
#define VEC_SHADE_INIT AVX_LINEAR_SHADE_INIT
#define VEC_SHADE_INIT4 AVX_LINEAR_SHADE_INIT4
#define VEC_SHADE AVX_LINEAR_SHADE
#include "r_draw_rgba_sse.h"
*/
#endif

/////////////////////////////////////////////////////////////////////////////

DrawerCommandQueue *DrawerCommandQueue::Instance()
{
	static DrawerCommandQueue queue;
	return &queue;
}

DrawerCommandQueue::~DrawerCommandQueue()
{
	StopThreads();
}

void* DrawerCommandQueue::AllocMemory(size_t size)
{
	// Make sure allocations remain 16-byte aligned
	size = (size + 15) / 16 * 16;

	auto queue = Instance();
	if (queue->memorypool_pos + size > memorypool_size)
		return nullptr;

	void *data = queue->memorypool + queue->memorypool_pos;
	queue->memorypool_pos += size;
	return data;
}

void DrawerCommandQueue::Begin()
{
	auto queue = Instance();
	queue->Finish();
	queue->threaded_render++;
}

void DrawerCommandQueue::End()
{
	auto queue = Instance();
	queue->Finish();
	if (queue->threaded_render > 0)
		queue->threaded_render--;
}

void DrawerCommandQueue::WaitForWorkers()
{
	Instance()->Finish();
}

void DrawerCommandQueue::Finish()
{
	auto queue = Instance();
	if (queue->commands.empty())
		return;

	// Give worker threads something to do:

	std::unique_lock<std::mutex> start_lock(queue->start_mutex);
	queue->active_commands.swap(queue->commands);
	queue->run_id++;
	start_lock.unlock();

	queue->StartThreads();
	queue->start_condition.notify_all();

	// Do one thread ourselves:

	DrawerThread thread;
	thread.core = 0;
	thread.num_cores = queue->threads.size() + 1;

	for (int pass = 0; pass < queue->num_passes; pass++)
	{
		thread.pass_start_y = pass * queue->rows_in_pass;
		thread.pass_end_y = (pass + 1) * queue->rows_in_pass;
		if (pass + 1 == queue->num_passes)
			thread.pass_end_y = MAX(thread.pass_end_y, MAXHEIGHT);

		size_t size = queue->active_commands.size();
		for (size_t i = 0; i < size; i++)
		{
			auto &command = queue->active_commands[i];
			command->Execute(&thread);
		}
	}

	// Wait for everyone to finish:

	std::unique_lock<std::mutex> end_lock(queue->end_mutex);
	queue->end_condition.wait(end_lock, [&]() { return queue->finished_threads == queue->threads.size(); });

	// Clean up batch:

	for (auto &command : queue->active_commands)
		command->~DrawerCommand();
	queue->active_commands.clear();
	queue->memorypool_pos = 0;
	queue->finished_threads = 0;
}

void DrawerCommandQueue::StartThreads()
{
	if (!threads.empty())
		return;

	int num_threads = std::thread::hardware_concurrency();
	if (num_threads == 0)
		num_threads = 4;

	threads.resize(num_threads - 1);

	for (int i = 0; i < num_threads - 1; i++)
	{
		DrawerCommandQueue *queue = this;
		DrawerThread *thread = &threads[i];
		thread->core = i + 1;
		thread->num_cores = num_threads;
		thread->thread = std::thread([=]()
		{
			int run_id = 0;
			while (true)
			{
				// Wait until we are signalled to run:
				std::unique_lock<std::mutex> start_lock(queue->start_mutex);
				queue->start_condition.wait(start_lock, [&]() { return queue->run_id != run_id || queue->shutdown_flag; });
				if (queue->shutdown_flag)
					break;
				run_id = queue->run_id;
				start_lock.unlock();

				// Do the work:
				for (int pass = 0; pass < queue->num_passes; pass++)
				{
					thread->pass_start_y = pass * queue->rows_in_pass;
					thread->pass_end_y = (pass + 1) * queue->rows_in_pass;
					if (pass + 1 == queue->num_passes)
						thread->pass_end_y = MAX(thread->pass_end_y, MAXHEIGHT);

					size_t size = queue->active_commands.size();
					for (size_t i = 0; i < size; i++)
					{
						auto &command = queue->active_commands[i];
						command->Execute(thread);
					}
				}

				// Notify main thread that we finished:
				std::unique_lock<std::mutex> end_lock(queue->end_mutex);
				queue->finished_threads++;
				end_lock.unlock();
				queue->end_condition.notify_all();
			}
		});
	}
}

void DrawerCommandQueue::StopThreads()
{
	std::unique_lock<std::mutex> lock(start_mutex);
	shutdown_flag = true;
	lock.unlock();
	start_condition.notify_all();
	for (auto &thread : threads)
		thread.thread.join();
	threads.clear();
	lock.lock();
	shutdown_flag = false;
}

/////////////////////////////////////////////////////////////////////////////

class DrawerColumnCommand : public DrawerCommand
{
public:
	int _count;
	BYTE * RESTRICT _dest;
	int _pitch;
	DWORD _iscale;
	DWORD _texturefrac;

	DrawerColumnCommand()
	{
		_count = dc_count;
		_dest = dc_dest;
		_iscale = dc_iscale;
		_texturefrac = dc_texturefrac;
		_pitch = dc_pitch;
	}

	class LoopIterator
	{
	public:
		int count;
		uint32_t *dest;
		int pitch;
		fixed_t fracstep;
		fixed_t frac;

		LoopIterator(DrawerColumnCommand *command, DrawerThread *thread)
		{
			count = thread->count_for_thread(command->_dest_y, command->_count);
			if (count <= 0)
				return;

			dest = thread->dest_for_thread(command->_dest_y, command->_pitch, (uint32_t*)command->_dest);
			pitch = command->_pitch * thread->num_cores;

			fracstep = command->_iscale * thread->num_cores;
			frac = command->_texturefrac + command->_iscale * thread->skipped_by_thread(command->_dest_y);
		}

		uint32_t sample_index()
		{
			return frac >> FRACBITS;
		}

		explicit operator bool()
		{
			return count > 0;
		}

		bool next()
		{
			dest += pitch;
			frac += fracstep;
			return (--count) != 0;
		}
	};
};

class DrawColumnRGBACommand : public DrawerColumnCommand
{
	uint32_t _light;
	const BYTE * RESTRICT _source;
	ShadeConstants _shade_constants;
	BYTE * RESTRICT _colormap;

public:
	DrawColumnRGBACommand()
	{
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_source = dc_source;
		_colormap = dc_colormap;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_colormap[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::copy(fg);
		} while (loop.next());
	}
};

class FillColumnRGBACommand : public DrawerColumnCommand
{
	uint32_t _color;

public:
	FillColumnRGBACommand()
	{
		uint32_t light = LightBgra::calc_light_multiplier(dc_light);
		_color = LightBgra::shade_pal_index_simple(dc_color, light);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			*loop.dest = BlendBgra::copy(_color);
		} while (loop.next());
	}
};

class FillAddColumnRGBACommand : public DrawerColumnCommand
{
	uint32_t _srccolor;

public:
	FillAddColumnRGBACommand()
	{
		_srccolor = dc_srccolor_bgra;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		uint32_t alpha = APART(_srccolor);
		alpha += alpha >> 7;

		do
		{
			*loop.dest = BlendBgra::add(_srccolor, *loop.dest, alpha, 256 - alpha);
		} while (loop.next());
	}
};

class FillAddClampColumnRGBACommand : public DrawerColumnCommand
{
	int _color;
	uint32_t _srccolor;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	FillAddClampColumnRGBACommand()
	{
		_color = dc_color;
		_srccolor = dc_srccolor_bgra;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			*loop.dest = BlendBgra::add(_srccolor, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class FillSubClampColumnRGBACommand : public DrawerColumnCommand
{
	uint32_t _srccolor;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	FillSubClampColumnRGBACommand()
	{
		_srccolor = dc_srccolor_bgra;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			*loop.dest = BlendBgra::sub(_srccolor, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class FillRevSubClampColumnRGBACommand : public DrawerColumnCommand
{
	uint32_t _srccolor;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	FillRevSubClampColumnRGBACommand()
	{
		_srccolor = dc_srccolor_bgra;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			*loop.dest = BlendBgra::revsub(_srccolor, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawAddColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;
	BYTE * RESTRICT _colormap;

public:
	DrawAddColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
		_colormap = dc_colormap;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_colormap[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawTranslatedColumnRGBACommand : public DrawerColumnCommand
{
	fixed_t _light;
	ShadeConstants _shade_constants;
	BYTE * RESTRICT _translation;
	const BYTE * RESTRICT _source;

public:
	DrawTranslatedColumnRGBACommand()
	{
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_translation = dc_translation;
		_source = dc_source;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_translation[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::copy(fg);
		} while (loop.next());
	}
};

class DrawTlatedAddColumnRGBACommand : public DrawerColumnCommand
{
	fixed_t _light;
	ShadeConstants _shade_constants;
	BYTE * RESTRICT _translation;
	const BYTE * RESTRICT _source;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	DrawTlatedAddColumnRGBACommand()
	{
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_translation = dc_translation;
		_source = dc_source;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_translation[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawShadedColumnRGBACommand : public DrawerColumnCommand
{
private:
	const BYTE * RESTRICT _source;
	lighttable_t * RESTRICT _colormap;
	uint32_t _color;

public:
	DrawShadedColumnRGBACommand()
	{
		_source = dc_source;
		_colormap = dc_colormap;
		_color = LightBgra::shade_pal_index_simple(dc_color, LightBgra::calc_light_multiplier(dc_light));
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t alpha = clamp<uint32_t>(_colormap[_source[loop.sample_index()]], 0, 64) * 4;
			uint32_t inv_alpha = 256 - alpha;
			*loop.dest = BlendBgra::add(_color, *loop.dest, alpha, inv_alpha);
		} while (loop.next());
	}
};

class DrawAddClampColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	DrawAddClampColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawAddClampTranslatedColumnRGBACommand : public DrawerColumnCommand
{
	BYTE * RESTRICT _translation;
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	DrawAddClampTranslatedColumnRGBACommand()
	{
		_translation = dc_translation;
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_translation[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawSubClampColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	DrawSubClampColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::sub(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawSubClampTranslatedColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;
	BYTE * RESTRICT _translation;

public:
	DrawSubClampTranslatedColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
		_translation = dc_translation;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_translation[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::sub(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawRevSubClampColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;

public:
	DrawRevSubClampColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::revsub(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawRevSubClampTranslatedColumnRGBACommand : public DrawerColumnCommand
{
	const BYTE * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	uint32_t _srcalpha;
	uint32_t _destalpha;
	BYTE * RESTRICT _translation;

public:
	DrawRevSubClampTranslatedColumnRGBACommand()
	{
		_source = dc_source;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
		_translation = dc_translation;
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_pal_index(_translation[_source[loop.sample_index()]], _light, _shade_constants);
			*loop.dest = BlendBgra::revsub(fg, *loop.dest, _srcalpha, _destalpha);
		} while (loop.next());
	}
};

class DrawFuzzColumnRGBACommand : public DrawerCommand
{
	int _x;
	int _yl;
	int _yh;
	BYTE * RESTRICT _destorg;
	int _pitch;
	int _fuzzpos;
	int _fuzzviewheight;

public:
	DrawFuzzColumnRGBACommand()
	{
		_x = dc_x;
		_yl = dc_yl;
		_yh = dc_yh;
		_destorg = dc_destorg;
		_pitch = dc_pitch;
		_fuzzpos = fuzzpos;
		_fuzzviewheight = fuzzviewheight;
	}

	void Execute(DrawerThread *thread) override
	{
		int yl = MAX(_yl, 1);
		int yh = MIN(_yh, _fuzzviewheight);

		int count = thread->count_for_thread(yl, yh - yl + 1);

		// Zero length.
		if (count <= 0)
			return;

		uint32_t *dest = thread->dest_for_thread(yl, _pitch, ylookup[yl] + _x + (uint32_t*)_destorg);

		int pitch = _pitch * thread->num_cores;
		int fuzzstep = thread->num_cores;
		int fuzz = (_fuzzpos + thread->skipped_by_thread(yl)) % FUZZTABLE;

		yl += thread->skipped_by_thread(yl);

		// Handle the case where we would go out of bounds at the top:
		if (yl < fuzzstep)
		{
			uint32_t bg = dest[fuzzoffset[fuzz] * fuzzstep + pitch];

			uint32_t red = RPART(bg) * 3 / 4;
			uint32_t green = GPART(bg) * 3 / 4;
			uint32_t blue = BPART(bg) * 3 / 4;

			*dest = 0xff000000 | (red << 16) | (green << 8) | blue;
			dest += pitch;
			fuzz += fuzzstep;
			fuzz %= FUZZTABLE;

			count--;
			if (count == 0)
				return;
		}

		bool lowerbounds = (yl + count * fuzzstep > _fuzzviewheight);
		if (lowerbounds)
			count--;

		// Fuzz where fuzzoffset stays within bounds
		while (count > 0)
		{
			int available = (FUZZTABLE - fuzz);
			int next_wrap = available / fuzzstep;
			if (available % fuzzstep != 0)
				next_wrap++;

			int cnt = MIN(count, next_wrap);
			count -= cnt;
			do
			{
				uint32_t bg = dest[fuzzoffset[fuzz] * fuzzstep];

				uint32_t red = RPART(bg) * 3 / 4;
				uint32_t green = GPART(bg) * 3 / 4;
				uint32_t blue = BPART(bg) * 3 / 4;

				*dest = 0xff000000 | (red << 16) | (green << 8) | blue;
				dest += pitch;
				fuzz += fuzzstep;
			} while (--cnt);

			fuzz %= FUZZTABLE;
		}

		// Handle the case where we would go out of bounds at the bottom
		if (lowerbounds)
		{
			uint32_t bg = dest[fuzzoffset[fuzz] * fuzzstep - pitch];

			uint32_t red = RPART(bg) * 3 / 4;
			uint32_t green = GPART(bg) * 3 / 4;
			uint32_t blue = BPART(bg) * 3 / 4;

			*dest = 0xff000000 | (red << 16) | (green << 8) | blue;
		}
	}
};

/////////////////////////////////////////////////////////////////////////////

class DrawerSpanCommand : public DrawerCommand
{
public:
	fixed_t _xfrac;
	fixed_t _yfrac;
	fixed_t _xstep;
	fixed_t _ystep;
	int _x1;
	int _x2;
	int _y;
	int _xbits;
	int _ybits;
	BYTE * RESTRICT _destorg;

	const uint32_t * RESTRICT _source;
	uint32_t _light;
	ShadeConstants _shade_constants;
	bool _magnifying;

	uint32_t _srcalpha;
	uint32_t _destalpha;

	DrawerSpanCommand()
	{
		_xfrac = ds_xfrac;
		_yfrac = ds_yfrac;
		_xstep = ds_xstep;
		_ystep = ds_ystep;
		_x1 = ds_x1;
		_x2 = ds_x2;
		_y = ds_y;
		_xbits = ds_xbits;
		_ybits = ds_ybits;
		_destorg = dc_destorg;

		_source = (const uint32_t*)ds_source;
		_light = LightBgra::calc_light_multiplier(ds_light);
		_shade_constants = ds_shade_constants;
		_magnifying = !SampleBgra::span_sampler_setup(_source, _xbits, _ybits, _xstep, _ystep);

		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	class LoopIterator
	{
	public:
		uint32_t *dest;
		int count;
		dsfixed_t xfrac;
		dsfixed_t yfrac;
		dsfixed_t xstep;
		dsfixed_t ystep;
		BYTE yshift;
		BYTE xshift;
		int xmask;
		bool is_64x64;
		bool skipped;

		LoopIterator(DrawerSpanCommand *command, DrawerThread *thread)
		{
			dest = ylookup[command->_y] + command->_x1 + (uint32_t*)command->_destorg;
			count = command->_x2 - command->_x1 + 1;
			xfrac = command->_xfrac;
			yfrac = command->_yfrac;
			xstep = command->_xstep;
			ystep = command->_ystep;
			yshift = 32 - command->_ybits;
			xshift = yshift - command->_xbits;
			xmask = ((1 << command->_xbits) - 1) << command->_ybits;
			is_64x64 = command->_xbits == 6 && command->_ybits == 6;
			skipped = thread->line_skipped_by_thread(command->_y);
		}

		// 64x64 is the most common case by far, so special case it.
		int spot64()
		{
			return ((xfrac >> (32 - 6 - 6))&(63 * 64)) + (yfrac >> (32 - 6));
		}

		int spot()
		{
			return ((xfrac >> xshift) & xmask) + (yfrac >> yshift);
		}

		explicit operator bool()
		{
			return !skipped && count > 0;
		}

		bool next()
		{
			dest++;
			xfrac += xstep;
			yfrac += ystep;
			return (--count) != 0;
		}
	};
};

class DrawSpanRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_magnifying)
		{
			if (loop.is_64x64)
			{
				do
				{
					*loop.dest = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
				} while (loop.next());
			}
			else
			{
				do
				{
					*loop.dest = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
				} while (loop.next());
			}
		}
		else
		{
			if (loop.is_64x64)
			{
				do
				{
					*loop.dest = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, loop.xfrac, loop.yfrac, 26, 26), _light, _shade_constants);
				} while (loop.next());
			}
			else
			{
				do
				{
					*loop.dest = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, loop.xfrac, loop.yfrac, 32 - _xbits, 32 - _ybits), _light, _shade_constants);
				} while (loop.next());
			}
		}
	}
};

class DrawSpanMaskedRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_magnifying)
		{
			if (loop.is_64x64)
			{
				do
				{
					uint32_t fg = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
					*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
				} while (loop.next());
			}
			else
			{
				do
				{
					uint32_t fg = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
					*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
				} while (loop.next());
			}
		}
		else
		{
			if (loop.is_64x64)
			{
				do
				{
					uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, loop.xfrac, loop.yfrac, 26, 26), _light, _shade_constants);
					*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
				} while (loop.next());
			}
			else
			{
				do
				{
					uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, loop.xfrac, loop.yfrac, 32 - _xbits, 32 - _ybits), _light, _shade_constants);
					*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
				} while (loop.next());
			}
		}
	}
};

class DrawSpanTranslucentRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (loop.is_64x64)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
			} while (loop.next());
		}
	}
};

class DrawSpanMaskedTranslucentRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (loop.is_64x64)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			} while (loop.next());
		}
	}
};

class DrawSpanAddClampRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (loop.is_64x64)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, _destalpha);
			} while (loop.next());
		}
	}
};

class DrawSpanMaskedAddClampRGBACommand : public DrawerSpanCommand
{
public:
	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (loop.is_64x64)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot64()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.spot()], _light, _shade_constants);
				*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			} while (loop.next());
		}
	}
};

class FillSpanRGBACommand : public DrawerCommand
{
	int _x1;
	int _x2;
	int _y;
	BYTE * RESTRICT _destorg;
	fixed_t _light;
	int _color;

public:
	FillSpanRGBACommand()
	{
		_x1 = ds_x1;
		_x2 = ds_x2;
		_y = ds_y;
		_destorg = dc_destorg;
		_light = ds_light;
		_color = ds_color;
	}

	void Execute(DrawerThread *thread) override
	{
		if (thread->line_skipped_by_thread(_y))
			return;

		uint32_t *dest = ylookup[_y] + _x1 + (uint32_t*)_destorg;
		int count = (_x2 - _x1 + 1);
		uint32_t light = LightBgra::calc_light_multiplier(_light);
		uint32_t color = LightBgra::shade_pal_index_simple(_color, light);
		for (int i = 0; i < count; i++)
			dest[i] = color;
	}
};

/////////////////////////////////////////////////////////////////////////////

class DrawSlabRGBACommand : public DrawerCommand
{
	int _dx;
	fixed_t _v;
	int _dy;
	fixed_t _vi;
	const BYTE *_voxelptr;
	uint32_t *_p;
	ShadeConstants _shade_constants;
	const BYTE *_colormap;
	fixed_t _light;
	int _pitch;
	int _start_y;

public:
	DrawSlabRGBACommand(int dx, fixed_t v, int dy, fixed_t vi, const BYTE *vptr, BYTE *p, ShadeConstants shade_constants, const BYTE *colormap, fixed_t light)
	{
		_dx = dx;
		_v = v;
		_dy = dy;
		_vi = vi;
		_voxelptr = vptr;
		_p = (uint32_t *)p;
		_shade_constants = shade_constants;
		_colormap = colormap;
		_light = light;
		_pitch = dc_pitch;
		_start_y = static_cast<int>((p - dc_destorg) / (dc_pitch * 4));
		assert(dx > 0);
	}

	void Execute(DrawerThread *thread) override
	{
		int dx = _dx;
		fixed_t v = _v;
		int dy = _dy;
		fixed_t vi = _vi;
		const BYTE *vptr = _voxelptr;
		uint32_t *p = _p;
		ShadeConstants shade_constants = _shade_constants;
		const BYTE *colormap = _colormap;
		uint32_t light = LightBgra::calc_light_multiplier(_light);
		int pitch = _pitch;
		int x;

		dy = thread->count_for_thread(_start_y, dy);
		p = thread->dest_for_thread(_start_y, pitch, p);
		v += vi * thread->skipped_by_thread(_start_y);
		vi *= thread->num_cores;
		pitch *= thread->num_cores;

		if (dx == 1)
		{
			while (dy > 0)
			{
				*p = LightBgra::shade_pal_index(colormap[vptr[v >> FRACBITS]], light, shade_constants);
				p += pitch;
				v += vi;
				dy--;
			}
		}
		else if (dx == 2)
		{
			while (dy > 0)
			{
				uint32_t color = LightBgra::shade_pal_index(colormap[vptr[v >> FRACBITS]], light, shade_constants);
				p[0] = color;
				p[1] = color;
				p += pitch;
				v += vi;
				dy--;
			}
		}
		else if (dx == 3)
		{
			while (dy > 0)
			{
				uint32_t color = LightBgra::shade_pal_index(colormap[vptr[v >> FRACBITS]], light, shade_constants);
				p[0] = color;
				p[1] = color;
				p[2] = color;
				p += pitch;
				v += vi;
				dy--;
			}
		}
		else if (dx == 4)
		{
			while (dy > 0)
			{
				uint32_t color = LightBgra::shade_pal_index(colormap[vptr[v >> FRACBITS]], light, shade_constants);
				p[0] = color;
				p[1] = color;
				p[2] = color;
				p[3] = color;
				p += pitch;
				v += vi;
				dy--;
			}
		}
		else while (dy > 0)
		{
			uint32_t color = LightBgra::shade_pal_index(colormap[vptr[v >> FRACBITS]], light, shade_constants);
			// The optimizer will probably turn this into a memset call.
			// Since dx is not likely to be large, I'm not sure that's a good thing,
			// hence the alternatives above.
			for (x = 0; x < dx; x++)
			{
				p[x] = color;
			}
			p += pitch;
			v += vi;
			dy--;
		}
	}
};

/////////////////////////////////////////////////////////////////////////////

class DrawerWall1Command : public DrawerCommand
{
public:
	BYTE * RESTRICT _dest;
	int _pitch;
	int _count;
	DWORD _texturefrac;
	uint32_t _texturefracx;
	DWORD _iscale;
	int _vlinebits;
	uint32_t _vlinemax;

	const uint32 * RESTRICT _source;
	const uint32 * RESTRICT _source2;
	uint32_t _light;
	ShadeConstants _shade_constants;

	uint32_t _srcalpha;
	uint32_t _destalpha;

	DrawerWall1Command(int vlinebits, uint32_t vlinemax)
	{
		_dest = dc_dest;
		_pitch = dc_pitch;
		_count = dc_count;
		_texturefrac = dc_texturefrac;
		_texturefracx = dc_texturefracx;
		_iscale = dc_iscale;
		_vlinebits = vlinebits;
		_vlinemax = vlinemax;

		_source = (const uint32 *)dc_source;
		_source2 = (const uint32 *)dc_source2;
		_light = LightBgra::calc_light_multiplier(dc_light);
		_shade_constants = dc_shade_constants;

		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	class LoopIterator
	{
	public:
		uint32_t *dest;
		int pitch;
		int count;
		uint32_t fracstep;
		uint32_t frac;
		uint32_t texturefracx;
		int bits;

		LoopIterator(DrawerWall1Command *command, DrawerThread *thread)
		{
			count = thread->count_for_thread(command->_dest_y, command->_count);
			if (count <= 0)
				return;

			fracstep = command->_iscale * thread->num_cores;
			frac = command->_texturefrac + command->_iscale * thread->skipped_by_thread(command->_dest_y);
			texturefracx = command->_texturefracx;
			dest = thread->dest_for_thread(command->_dest_y, command->_pitch, (uint32_t*)command->_dest);
			bits = command->_vlinebits;
			pitch = command->_pitch * thread->num_cores;
		}

		explicit operator bool()
		{
			return count > 0;
		}

		int sample_index()
		{
			return frac >> bits;
		}

		bool next()
		{
			frac += fracstep;
			dest += pitch;
			return (--count) != 0;
		}
	};
};

class DrawerWall4Command : public DrawerCommand
{
public:
	BYTE * RESTRICT _dest;
	int _count;
	int _pitch;
	int _vlinebits;
	uint32_t _vlinemax;
	ShadeConstants _shade_constants;
	uint32_t _vplce[4];
	uint32_t _vince[4];
	uint32_t _buftexturefracx[4];
	const uint32_t * RESTRICT _bufplce[4];
	const uint32_t * RESTRICT _bufplce2[4];
	uint32_t _light[4];

	uint32_t _srcalpha;
	uint32_t _destalpha;

	DrawerWall4Command(int vlinebits, uint32_t vlinemax)
	{
		_dest = dc_dest;
		_count = dc_count;
		_pitch = dc_pitch;
		_vlinebits = vlinebits;
		_vlinemax = vlinemax;
		_shade_constants = dc_shade_constants;
		for (int i = 0; i < 4; i++)
		{
			_vplce[i] = vplce[i];
			_vince[i] = vince[i];
			_buftexturefracx[i] = buftexturefracx[i];
			_bufplce[i] = (const uint32_t *)bufplce[i];
			_bufplce2[i] = (const uint32_t *)bufplce2[i];
			_light[i] = LightBgra::calc_light_multiplier(palookuplight[i]);
		}
		_srcalpha = dc_srcalpha >> (FRACBITS - 8);
		_destalpha = dc_destalpha >> (FRACBITS - 8);
	}

	class LoopIterator
	{
	public:
		uint32_t *dest;
		int pitch;
		int count;
		int bits;
		uint32_t vplce[4];
		uint32_t vince[4];

		LoopIterator(DrawerWall4Command *command, DrawerThread *thread)
		{
			count = thread->count_for_thread(command->_dest_y, command->_count);
			if (count <= 0)
				return;

			dest = thread->dest_for_thread(command->_dest_y, command->_pitch, (uint32_t*)command->_dest);
			pitch = command->_pitch * thread->num_cores;
			bits = command->_vlinebits;

			int skipped = thread->skipped_by_thread(command->_dest_y);
			for (int i = 0; i < 4; i++)
			{
				vplce[i] = command->_vplce[i] + command->_vince[i] * skipped;
				vince[i] = command->_vince[i] * thread->num_cores;
			}
		}

		explicit operator bool()
		{
			return count > 0;
		}

		int sample_index(int col)
		{
			return vplce[col] >> bits;
		}

		bool next()
		{
			vplce[0] += vince[0];
			vplce[1] += vince[1];
			vplce[2] += vince[2];
			vplce[3] += vince[3];
			dest += pitch;
			return (--count) != 0;
		}
	};
};

class Vlinec1RGBACommand : public DrawerWall1Command
{
public:
	Vlinec1RGBACommand() : DrawerWall1Command(vlinebits, vlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_source2 == nullptr)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
				*loop.dest = BlendBgra::copy(fg);
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, _source2, loop.texturefracx, loop.frac, loop.bits, _vlinemax), _light, _shade_constants);
				*loop.dest = BlendBgra::copy(fg);
			} while (loop.next());
		}
	}
};

class Vlinec4RGBACommand : public DrawerWall4Command
{
public:
	Vlinec4RGBACommand() : DrawerWall4Command(vlinebits, vlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_bufplce2[0] == nullptr)
		{
			do
			{
				for (int i = 0; i < 4; i++)
				{
					uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
					loop.dest[i] = BlendBgra::copy(fg);
				}
			} while (loop.next());
		}
		else
		{
			do
			{
				for (int i = 0; i < 4; i++)
				{
					uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_bufplce[i], _bufplce2[i], _buftexturefracx[i], loop.sample_index(i), loop.bits, _vlinemax), _light[i], _shade_constants);
					loop.dest[i] = BlendBgra::copy(fg);
				}
			} while (loop.next());
		}
	}
};

class Mvlinec1RGBACommand : public DrawerWall1Command
{
public:
	Mvlinec1RGBACommand() : DrawerWall1Command(mvlinebits, mvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_source2 == nullptr)
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
				*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
			} while (loop.next());
		}
		else
		{
			do
			{
				uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_source, _source2, loop.texturefracx, loop.frac, loop.bits, _vlinemax), _light, _shade_constants);
				*loop.dest = BlendBgra::alpha_blend(fg, *loop.dest);
			} while (loop.next());
		}
	}
};

class Mvlinec4RGBACommand : public DrawerWall4Command
{
public:
	Mvlinec4RGBACommand(): DrawerWall4Command(mvlinebits, mvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;

		if (_bufplce2[0] == nullptr)
		{
			do
			{
				for (int i = 0; i < 4; i++)
				{
					uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
					loop.dest[i] = BlendBgra::alpha_blend(fg, loop.dest[i]);
				}
			} while (loop.next());
		}
		else
		{
			do
			{
				for (int i = 0; i < 4; i++)
				{
					uint32_t fg = LightBgra::shade_bgra(SampleBgra::sample_bilinear(_bufplce[i], _bufplce2[i], _buftexturefracx[i], loop.sample_index(i), loop.bits, _vlinemax), _light[i], _shade_constants);
					loop.dest[i] = BlendBgra::alpha_blend(fg, loop.dest[i]);
				}
			} while (loop.next());
		}
	}
};

class Tmvline1AddRGBACommand : public DrawerWall1Command
{
public:
	Tmvline1AddRGBACommand() : DrawerWall1Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
		} while (loop.next());
	}
};

class Tmvline4AddRGBACommand : public DrawerWall4Command
{
public:
	Tmvline4AddRGBACommand() : DrawerWall4Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			for (int i = 0; i < 4; i++)
			{
				uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
				loop.dest[i] = BlendBgra::add(fg, loop.dest[i], _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			}
		} while (loop.next());
	}
};

class Tmvline1AddClampRGBACommand : public DrawerWall1Command
{
public:
	Tmvline1AddClampRGBACommand() : DrawerWall1Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::add(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
		} while (loop.next());
	}
};

class Tmvline4AddClampRGBACommand : public DrawerWall4Command
{
public:
	Tmvline4AddClampRGBACommand() : DrawerWall4Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			for (int i = 0; i < 4; i++)
			{
				uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
				loop.dest[i] = BlendBgra::add(fg, loop.dest[i], _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			}
		} while (loop.next());
	}
};

class Tmvline1SubClampRGBACommand : public DrawerWall1Command
{
public:
	Tmvline1SubClampRGBACommand() : DrawerWall1Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::sub(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
		} while (loop.next());
	}
};

class Tmvline4SubClampRGBACommand : public DrawerWall4Command
{
public:
	Tmvline4SubClampRGBACommand() : DrawerWall4Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			for (int i = 0; i < 4; i++)
			{
				uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
				loop.dest[i] = BlendBgra::sub(fg, loop.dest[i], _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			}
		} while (loop.next());
	}
};

class Tmvline1RevSubClampRGBACommand : public DrawerWall1Command
{
public:
	Tmvline1RevSubClampRGBACommand() : DrawerWall1Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			uint32_t fg = LightBgra::shade_bgra(_source[loop.sample_index()], _light, _shade_constants);
			*loop.dest = BlendBgra::revsub(fg, *loop.dest, _srcalpha, calc_blend_bgalpha(fg, _destalpha));
		} while (loop.next());
	}
};

class Tmvline4RevSubClampRGBACommand : public DrawerWall4Command
{
public:
	Tmvline4RevSubClampRGBACommand() : DrawerWall4Command(tmvlinebits, tmvlinemax)
	{
	}

	void Execute(DrawerThread *thread) override
	{
		LoopIterator loop(this, thread);
		if (!loop) return;
		do
		{
			for (int i = 0; i < 4; i++)
			{
				uint32_t fg = LightBgra::shade_bgra(_bufplce[i][loop.sample_index(i)], _light[i], _shade_constants);
				loop.dest[i] = BlendBgra::revsub(fg, loop.dest[i], _srcalpha, calc_blend_bgalpha(fg, _destalpha));
			}
		} while (loop.next());
	}
};

/////////////////////////////////////////////////////////////////////////////

class DrawFogBoundaryLineRGBACommand : public DrawerCommand
{
	int _y;
	int _x;
	int _x2;
	BYTE * RESTRICT _destorg;
	fixed_t _light;
	ShadeConstants _shade_constants;

public:
	DrawFogBoundaryLineRGBACommand(int y, int x, int x2)
	{
		_y = y;
		_x = x;
		_x2 = x2;

		_destorg = dc_destorg;
		_light = dc_light;
		_shade_constants = dc_shade_constants;
	}

	void Execute(DrawerThread *thread) override
	{
		if (thread->line_skipped_by_thread(_y))
			return;

		int y = _y;
		int x = _x;
		int x2 = _x2;

		uint32_t *dest = ylookup[y] + (uint32_t*)_destorg;

		uint32_t light = LightBgra::calc_light_multiplier(_light);
		ShadeConstants constants = _shade_constants;

		do
		{
			uint32_t red = (dest[x] >> 16) & 0xff;
			uint32_t green = (dest[x] >> 8) & 0xff;
			uint32_t blue = dest[x] & 0xff;

			if (constants.simple_shade)
			{
				red = red * light / 256;
				green = green * light / 256;
				blue = blue * light / 256;
			}
			else
			{
				uint32_t inv_light = 256 - light;
				uint32_t inv_desaturate = 256 - constants.desaturate;

				uint32_t intensity = ((red * 77 + green * 143 + blue * 37) >> 8) * constants.desaturate;

				red = (red * inv_desaturate + intensity) / 256;
				green = (green * inv_desaturate + intensity) / 256;
				blue = (blue * inv_desaturate + intensity) / 256;

				red = (constants.fade_red * inv_light + red * light) / 256;
				green = (constants.fade_green * inv_light + green * light) / 256;
				blue = (constants.fade_blue * inv_light + blue * light) / 256;

				red = (red * constants.light_red) / 256;
				green = (green * constants.light_green) / 256;
				blue = (blue * constants.light_blue) / 256;
			}

			dest[x] = 0xff000000 | (red << 16) | (green << 8) | blue;
		} while (++x <= x2);
	}
};

class DrawTiltedSpanRGBACommand : public DrawerCommand
{
	int _y;
	int _x1;
	int _x2;
	BYTE * RESTRICT _destorg;
	fixed_t _light;
	ShadeConstants _shade_constants;
	const BYTE * RESTRICT _source;

public:
	DrawTiltedSpanRGBACommand(int y, int x1, int x2)
	{
		_y = y;
		_x1 = x1;
		_x2 = x2;

		_destorg = dc_destorg;
		_source = ds_source;
	}

	void Execute(DrawerThread *thread) override
	{
		if (thread->line_skipped_by_thread(_y))
			return;

		int y = _y;
		int x1 = _x1;
		int x2 = _x2;

		// Slopes are broken currently in master.
		// Until R_DrawTiltedPlane is fixed we are just going to fill with a solid color.

		uint32_t *source = (uint32_t*)_source;
		uint32_t *dest = ylookup[y] + x1 + (uint32_t*)_destorg;

		int count = x2 - x1 + 1;
		while (count > 0)
		{
			*(dest++) = source[0];
			count--;
		}
	}
};

class DrawColoredSpanRGBACommand : public DrawerCommand
{
	int _y;
	int _x1;
	int _x2;
	BYTE * RESTRICT _destorg;
	fixed_t _light;
	int _color;

public:
	DrawColoredSpanRGBACommand(int y, int x1, int x2)
	{
		_y = y;
		_x1 = x1;
		_x2 = x2;

		_destorg = dc_destorg;
		_light = ds_light;
		_color = ds_color;
	}

	void Execute(DrawerThread *thread) override
	{
		if (thread->line_skipped_by_thread(_y))
			return;

		int y = _y;
		int x1 = _x1;
		int x2 = _x2;

		uint32_t *dest = ylookup[y] + x1 + (uint32_t*)_destorg;
		int count = (x2 - x1 + 1);
		uint32_t light = LightBgra::calc_light_multiplier(_light);
		uint32_t color = LightBgra::shade_pal_index_simple(_color, light);
		for (int i = 0; i < count; i++)
			dest[i] = color;
	}
};

class FillTransColumnRGBACommand : public DrawerCommand
{
	int _x;
	int _y1;
	int _y2;
	int _color;
	int _a;
	BYTE * RESTRICT _destorg;
	int _pitch;
	fixed_t _light;

public:
	FillTransColumnRGBACommand(int x, int y1, int y2, int color, int a)
	{
		_x = x;
		_y1 = y1;
		_y2 = y2;
		_color = color;
		_a = a;

		_destorg = dc_destorg;
		_pitch = dc_pitch;
	}

	void Execute(DrawerThread *thread) override
	{
		int x = _x;
		int y1 = _y1;
		int y2 = _y2;
		int color = _color;
		int a = _a;

		int ycount = thread->count_for_thread(y1, y2 - y1 + 1);
		if (ycount <= 0)
			return;

		uint32_t fg = GPalette.BaseColors[color].d;
		uint32_t fg_red = (fg >> 16) & 0xff;
		uint32_t fg_green = (fg >> 8) & 0xff;
		uint32_t fg_blue = fg & 0xff;

		uint32_t alpha = a + 1;
		uint32_t inv_alpha = 256 - alpha;

		fg_red *= alpha;
		fg_green *= alpha;
		fg_blue *= alpha;

		int spacing = _pitch * thread->num_cores;
		uint32_t *dest = thread->dest_for_thread(y1, _pitch, ylookup[y1] + x + (uint32_t*)_destorg);

		for (int y = 0; y < ycount; y++)
		{
			uint32_t bg_red = (*dest >> 16) & 0xff;
			uint32_t bg_green = (*dest >> 8) & 0xff;
			uint32_t bg_blue = (*dest) & 0xff;

			uint32_t red = (fg_red + bg_red * inv_alpha) / 256;
			uint32_t green = (fg_green + bg_green * inv_alpha) / 256;
			uint32_t blue = (fg_blue + bg_blue * inv_alpha) / 256;

			*dest = 0xff000000 | (red << 16) | (green << 8) | blue;
			dest += spacing;
		}
	}
};

ApplySpecialColormapRGBACommand::ApplySpecialColormapRGBACommand(FSpecialColormap *colormap, DFrameBuffer *screen)
{
	buffer = screen->GetBuffer();
	pitch = screen->GetPitch();
	width = screen->GetWidth();
	height = screen->GetHeight();

	start_red = (int)(colormap->ColorizeStart[0] * 255);
	start_green = (int)(colormap->ColorizeStart[1] * 255);
	start_blue = (int)(colormap->ColorizeStart[2] * 255);
	end_red = (int)(colormap->ColorizeEnd[0] * 255);
	end_green = (int)(colormap->ColorizeEnd[1] * 255);
	end_blue = (int)(colormap->ColorizeEnd[2] * 255);
}

#ifdef NO_SSE
void ApplySpecialColormapRGBACommand::Execute(DrawerThread *thread)
{
	int y = thread->skipped_by_thread(0);
	int count = thread->count_for_thread(0, height);
	while (count > 0)
	{
		BYTE *pixels = buffer + y * pitch * 4;
		for (int x = 0; x < width; x++)
		{
			int fg_red = pixels[2];
			int fg_green = pixels[1];
			int fg_blue = pixels[0];

			int gray = (fg_red * 77 + fg_green * 143 + fg_blue * 37) >> 8;
			gray += (gray >> 7); // gray*=256/255
			int inv_gray = 256 - gray;

			int red = clamp((start_red * inv_gray + end_red * gray) >> 8, 0, 255);
			int green = clamp((start_green * inv_gray + end_green * gray) >> 8, 0, 255);
			int blue = clamp((start_blue * inv_gray + end_blue * gray) >> 8, 0, 255);

			pixels[0] = (BYTE)blue;
			pixels[1] = (BYTE)green;
			pixels[2] = (BYTE)red;
			pixels[3] = 0xff;

			pixels += 4;
		}
		y += thread->num_cores;
		count--;
	}
}
#else
void ApplySpecialColormapRGBACommand::Execute(DrawerThread *thread)
{
	int y = thread->skipped_by_thread(0);
	int count = thread->count_for_thread(0, height);
	__m128i gray_weight = _mm_set_epi16(256, 77, 143, 37, 256, 77, 143, 37);
	__m128i start_end = _mm_set_epi16(255, start_red, start_green, start_blue, 255, end_red, end_green, end_blue);
	while (count > 0)
	{
		BYTE *pixels = buffer + y * pitch * 4;
		int sse_length = width / 4;
		for (int x = 0; x < sse_length; x++)
		{
			// Unpack to integers:
			__m128i p = _mm_loadu_si128((const __m128i*)pixels);

			__m128i p16_0 = _mm_unpacklo_epi8(p, _mm_setzero_si128());
			__m128i p16_1 = _mm_unpackhi_epi8(p, _mm_setzero_si128());

			// Add gray weighting to colors
			__m128i mullo0 = _mm_mullo_epi16(p16_0, gray_weight);
			__m128i mullo1 = _mm_mullo_epi16(p16_1, gray_weight);
			__m128i p32_0 = _mm_unpacklo_epi16(mullo0, _mm_setzero_si128());
			__m128i p32_1 = _mm_unpackhi_epi16(mullo0, _mm_setzero_si128());
			__m128i p32_2 = _mm_unpacklo_epi16(mullo1, _mm_setzero_si128());
			__m128i p32_3 = _mm_unpackhi_epi16(mullo1, _mm_setzero_si128());

			// Transpose to get color components in individual vectors:
			__m128 tmpx = _mm_castsi128_ps(p32_0);
			__m128 tmpy = _mm_castsi128_ps(p32_1);
			__m128 tmpz = _mm_castsi128_ps(p32_2);
			__m128 tmpw = _mm_castsi128_ps(p32_3);
			_MM_TRANSPOSE4_PS(tmpx, tmpy, tmpz, tmpw);
			__m128i blue = _mm_castps_si128(tmpx);
			__m128i green = _mm_castps_si128(tmpy);
			__m128i red = _mm_castps_si128(tmpz);
			__m128i alpha = _mm_castps_si128(tmpw);

			// Calculate gray and 256-gray values:
			__m128i gray = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(red, green), blue), 8);
			__m128i inv_gray = _mm_sub_epi32(_mm_set1_epi32(256), gray);

			// p32 = start * inv_gray + end * gray:
			__m128i gray0 = _mm_shuffle_epi32(gray, _MM_SHUFFLE(0, 0, 0, 0));
			__m128i gray1 = _mm_shuffle_epi32(gray, _MM_SHUFFLE(1, 1, 1, 1));
			__m128i gray2 = _mm_shuffle_epi32(gray, _MM_SHUFFLE(2, 2, 2, 2));
			__m128i gray3 = _mm_shuffle_epi32(gray, _MM_SHUFFLE(3, 3, 3, 3));
			__m128i inv_gray0 = _mm_shuffle_epi32(inv_gray, _MM_SHUFFLE(0, 0, 0, 0));
			__m128i inv_gray1 = _mm_shuffle_epi32(inv_gray, _MM_SHUFFLE(1, 1, 1, 1));
			__m128i inv_gray2 = _mm_shuffle_epi32(inv_gray, _MM_SHUFFLE(2, 2, 2, 2));
			__m128i inv_gray3 = _mm_shuffle_epi32(inv_gray, _MM_SHUFFLE(3, 3, 3, 3));
			__m128i gray16_0 = _mm_packs_epi32(gray0, inv_gray0);
			__m128i gray16_1 = _mm_packs_epi32(gray1, inv_gray1);
			__m128i gray16_2 = _mm_packs_epi32(gray2, inv_gray2);
			__m128i gray16_3 = _mm_packs_epi32(gray3, inv_gray3);
			__m128i gray16_0_mullo = _mm_mullo_epi16(gray16_0, start_end);
			__m128i gray16_1_mullo = _mm_mullo_epi16(gray16_1, start_end);
			__m128i gray16_2_mullo = _mm_mullo_epi16(gray16_2, start_end);
			__m128i gray16_3_mullo = _mm_mullo_epi16(gray16_3, start_end);
			__m128i gray16_0_mulhi = _mm_mulhi_epi16(gray16_0, start_end);
			__m128i gray16_1_mulhi = _mm_mulhi_epi16(gray16_1, start_end);
			__m128i gray16_2_mulhi = _mm_mulhi_epi16(gray16_2, start_end);
			__m128i gray16_3_mulhi = _mm_mulhi_epi16(gray16_3, start_end);
			p32_0 = _mm_srli_epi32(_mm_add_epi32(_mm_unpacklo_epi16(gray16_0_mullo, gray16_0_mulhi), _mm_unpackhi_epi16(gray16_0_mullo, gray16_0_mulhi)), 8);
			p32_1 = _mm_srli_epi32(_mm_add_epi32(_mm_unpacklo_epi16(gray16_1_mullo, gray16_1_mulhi), _mm_unpackhi_epi16(gray16_1_mullo, gray16_1_mulhi)), 8);
			p32_2 = _mm_srli_epi32(_mm_add_epi32(_mm_unpacklo_epi16(gray16_2_mullo, gray16_2_mulhi), _mm_unpackhi_epi16(gray16_2_mullo, gray16_2_mulhi)), 8);
			p32_3 = _mm_srli_epi32(_mm_add_epi32(_mm_unpacklo_epi16(gray16_3_mullo, gray16_3_mulhi), _mm_unpackhi_epi16(gray16_3_mullo, gray16_3_mulhi)), 8);

			p16_0 = _mm_packs_epi32(p32_0, p32_1);
			p16_1 = _mm_packs_epi32(p32_2, p32_3);
			p = _mm_packus_epi16(p16_0, p16_1);

			_mm_storeu_si128((__m128i*)pixels, p);
			pixels += 16;
		}

		for (int x = sse_length * 4; x < width; x++)
		{
			int fg_red = pixels[2];
			int fg_green = pixels[1];
			int fg_blue = pixels[0];

			int gray = (fg_red * 77 + fg_green * 143 + fg_blue * 37) >> 8;
			gray += (gray >> 7); // gray*=256/255
			int inv_gray = 256 - gray;

			int red = clamp((start_red * inv_gray + end_red * gray) >> 8, 0, 255);
			int green = clamp((start_green * inv_gray + end_green * gray) >> 8, 0, 255);
			int blue = clamp((start_blue * inv_gray + end_blue * gray) >> 8, 0, 255);

			pixels[0] = (BYTE)blue;
			pixels[1] = (BYTE)green;
			pixels[2] = (BYTE)red;
			pixels[3] = 0xff;

			pixels += 4;
		}

		y += thread->num_cores;
		count--;
	}
}
#endif

/////////////////////////////////////////////////////////////////////////////

void R_BeginDrawerCommands()
{
	DrawerCommandQueue::Begin();
}

void R_EndDrawerCommands()
{
	DrawerCommandQueue::End();
}

void R_DrawColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawColumnRGBACommand>();
}

void R_FillColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<FillColumnRGBACommand>();
}

void R_FillAddColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<FillAddColumnRGBACommand>();
}

void R_FillAddClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<FillAddClampColumnRGBACommand>();
}

void R_FillSubClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<FillSubClampColumnRGBACommand>();
}

void R_FillRevSubClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<FillRevSubClampColumnRGBACommand>();
}

void R_DrawFuzzColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawFuzzColumnRGBACommand>();

	dc_yl = MAX(dc_yl, 1);
	dc_yh = MIN(dc_yh, fuzzviewheight);
	if (dc_yl <= dc_yh)
		fuzzpos = (fuzzpos + dc_yh - dc_yl + 1) % FUZZTABLE;
}

void R_DrawAddColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawAddColumnRGBACommand>();
}

void R_DrawTranslatedColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawTranslatedColumnRGBACommand>();
}

void R_DrawTlatedAddColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawTlatedAddColumnRGBACommand>();
}

void R_DrawShadedColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawShadedColumnRGBACommand>();
}

void R_DrawAddClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawAddClampColumnRGBACommand>();
}

void R_DrawAddClampTranslatedColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawAddClampTranslatedColumnRGBACommand>();
}

void R_DrawSubClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSubClampColumnRGBACommand>();
}

void R_DrawSubClampTranslatedColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSubClampTranslatedColumnRGBACommand>();
}

void R_DrawRevSubClampColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawRevSubClampColumnRGBACommand>();
}

void R_DrawRevSubClampTranslatedColumn_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawRevSubClampTranslatedColumnRGBACommand>();
}

void R_DrawSpan_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<DrawSpanRGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<DrawSpanRGBA_SSE_Command>();
#endif
}

void R_DrawSpanMasked_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSpanMaskedRGBACommand>();
}

void R_DrawSpanTranslucent_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSpanTranslucentRGBACommand>();
}

void R_DrawSpanMaskedTranslucent_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSpanMaskedTranslucentRGBACommand>();
}

void R_DrawSpanAddClamp_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSpanAddClampRGBACommand>();
}

void R_DrawSpanMaskedAddClamp_rgba()
{
	DrawerCommandQueue::QueueCommand<DrawSpanMaskedAddClampRGBACommand>();
}

void R_FillSpan_rgba()
{
	DrawerCommandQueue::QueueCommand<FillSpanRGBACommand>();
}

static ShadeConstants slab_rgba_shade_constants;
static const BYTE *slab_rgba_colormap;
static fixed_t slab_rgba_light;

void R_SetupDrawSlab_rgba(FColormap *base_colormap, float light, int shade)
{
	slab_rgba_shade_constants.light_red = base_colormap->Color.r * 256 / 255;
	slab_rgba_shade_constants.light_green = base_colormap->Color.g * 256 / 255;
	slab_rgba_shade_constants.light_blue = base_colormap->Color.b * 256 / 255;
	slab_rgba_shade_constants.light_alpha = base_colormap->Color.a * 256 / 255;
	slab_rgba_shade_constants.fade_red = base_colormap->Fade.r;
	slab_rgba_shade_constants.fade_green = base_colormap->Fade.g;
	slab_rgba_shade_constants.fade_blue = base_colormap->Fade.b;
	slab_rgba_shade_constants.fade_alpha = base_colormap->Fade.a;
	slab_rgba_shade_constants.desaturate = MIN(abs(base_colormap->Desaturate), 255) * 255 / 256;
	slab_rgba_shade_constants.simple_shade = (base_colormap->Color.d == 0x00ffffff && base_colormap->Fade.d == 0x00000000 && base_colormap->Desaturate == 0);
	slab_rgba_colormap = base_colormap->Maps;
	slab_rgba_light = LIGHTSCALE(light, shade);
}

void R_DrawSlab_rgba(int dx, fixed_t v, int dy, fixed_t vi, const BYTE *vptr, BYTE *p)
{
	DrawerCommandQueue::QueueCommand<DrawSlabRGBACommand>(dx, v, dy, vi, vptr, p, slab_rgba_shade_constants, slab_rgba_colormap, slab_rgba_light);
}

//extern FTexture *rw_pic; // For the asserts below

DWORD vlinec1_rgba()
{
	/*DWORD fracstep = dc_iscale;
	DWORD frac = dc_texturefrac;
	DWORD height = rw_pic->GetHeight();
	assert((frac >> vlinebits) < height);
	frac += (dc_count-1) * fracstep;
	assert((frac >> vlinebits) <= height);*/

	DrawerCommandQueue::QueueCommand<Vlinec1RGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void vlinec4_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Vlinec4RGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Vlinec4RGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

DWORD mvlinec1_rgba()
{
	DrawerCommandQueue::QueueCommand<Mvlinec1RGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void mvlinec4_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Mvlinec4RGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Mvlinec4RGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

fixed_t tmvline1_add_rgba()
{
	DrawerCommandQueue::QueueCommand<Tmvline1AddRGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void tmvline4_add_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Tmvline4AddRGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Tmvline4AddRGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

fixed_t tmvline1_addclamp_rgba()
{
	DrawerCommandQueue::QueueCommand<Tmvline1AddClampRGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void tmvline4_addclamp_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Tmvline4AddClampRGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Tmvline4AddClampRGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

fixed_t tmvline1_subclamp_rgba()
{
	DrawerCommandQueue::QueueCommand<Tmvline1SubClampRGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void tmvline4_subclamp_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Tmvline4SubClampRGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Tmvline4SubClampRGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

fixed_t tmvline1_revsubclamp_rgba()
{
	DrawerCommandQueue::QueueCommand<Tmvline1RevSubClampRGBACommand>();
	return dc_texturefrac + dc_count * dc_iscale;
}

void tmvline4_revsubclamp_rgba()
{
#ifdef NO_SSE
	DrawerCommandQueue::QueueCommand<Tmvline4RevSubClampRGBACommand>();
#else
	DrawerCommandQueue::QueueCommand<Tmvline4RevSubClampRGBA_SSE_Command>();
#endif
	for (int i = 0; i < 4; i++)
		vplce[i] += vince[i] * dc_count;
}

void R_DrawFogBoundarySection_rgba(int y, int y2, int x1)
{
	for (; y < y2; ++y)
	{
		int x2 = spanend[y];
		DrawerCommandQueue::QueueCommand<DrawFogBoundaryLineRGBACommand>(y, x1, x2);
	}
}

void R_DrawFogBoundary_rgba(int x1, int x2, short *uclip, short *dclip)
{
	// To do: we do not need to create new spans when using rgba output - instead we should calculate light on a per pixel basis

	// This is essentially the same as R_MapVisPlane but with an extra step
	// to create new horizontal spans whenever the light changes enough that
	// we need to use a new colormap.

	double lightstep = rw_lightstep;
	double light = rw_light + rw_lightstep*(x2 - x1 - 1);
	int x = x2 - 1;
	int t2 = uclip[x];
	int b2 = dclip[x];
	int rcolormap = GETPALOOKUP(light, wallshade);
	int lcolormap;
	BYTE *basecolormapdata = basecolormap->Maps;

	if (b2 > t2)
	{
		clearbufshort(spanend + t2, b2 - t2, x);
	}

	R_SetColorMapLight(basecolormap, (float)light, wallshade);

	BYTE *fake_dc_colormap = basecolormap->Maps + (GETPALOOKUP(light, wallshade) << COLORMAPSHIFT);

	for (--x; x >= x1; --x)
	{
		int t1 = uclip[x];
		int b1 = dclip[x];
		const int xr = x + 1;
		int stop;

		light -= rw_lightstep;
		lcolormap = GETPALOOKUP(light, wallshade);
		if (lcolormap != rcolormap)
		{
			if (t2 < b2 && rcolormap != 0)
			{ // Colormap 0 is always the identity map, so rendering it is
			  // just a waste of time.
				R_DrawFogBoundarySection_rgba(t2, b2, xr);
			}
			if (t1 < t2) t2 = t1;
			if (b1 > b2) b2 = b1;
			if (t2 < b2)
			{
				clearbufshort(spanend + t2, b2 - t2, x);
			}
			rcolormap = lcolormap;
			R_SetColorMapLight(basecolormap, (float)light, wallshade);
			fake_dc_colormap = basecolormap->Maps + (GETPALOOKUP(light, wallshade) << COLORMAPSHIFT);
		}
		else
		{
			if (fake_dc_colormap != basecolormapdata)
			{
				stop = MIN(t1, b2);
				while (t2 < stop)
				{
					int y = t2++;
					DrawerCommandQueue::QueueCommand<DrawFogBoundaryLineRGBACommand>(y, xr, spanend[y]);
				}
				stop = MAX(b1, t2);
				while (b2 > stop)
				{
					int y = --b2;
					DrawerCommandQueue::QueueCommand<DrawFogBoundaryLineRGBACommand>(y, xr, spanend[y]);
				}
			}
			else
			{
				t2 = MAX(t2, MIN(t1, b2));
				b2 = MIN(b2, MAX(b1, t2));
			}

			stop = MIN(t2, b1);
			while (t1 < stop)
			{
				spanend[t1++] = x;
			}
			stop = MAX(b2, t2);
			while (b1 > stop)
			{
				spanend[--b1] = x;
			}
		}

		t2 = uclip[x];
		b2 = dclip[x];
	}
	if (t2 < b2 && rcolormap != 0)
	{
		R_DrawFogBoundarySection_rgba(t2, b2, x1);
	}
}
