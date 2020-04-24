/*
 * File writer filter
 *
 * Copyright (C) 2020 Zebediah Figura
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#include "dshow.h"
#include "qcap_main.h"
#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(qcap);

struct file_writer
{
    struct strmbase_filter filter;
    IFileSinkFilter IFileSinkFilter_iface;

    struct strmbase_sink sink;

    WCHAR *filename;
    HANDLE file;
};

static inline struct file_writer *impl_from_strmbase_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, struct file_writer, sink.pin);
}

static HRESULT file_writer_sink_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct file_writer *filter = impl_from_strmbase_pin(iface);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->sink.IMemInputPin_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT file_writer_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct file_writer *filter = impl_from_strmbase_pin(iface);

    if (filter->filename && !IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    return S_OK;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_interface = file_writer_sink_query_interface,
    .base.pin_query_accept = file_writer_sink_query_accept,
};

static inline struct file_writer *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct file_writer, filter);
}

static HRESULT file_writer_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct file_writer *filter = impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IFileSinkFilter))
        *out = &filter->IFileSinkFilter_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static struct strmbase_pin *file_writer_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct file_writer *filter = impl_from_strmbase_filter(iface);

    if (!index)
        return &filter->sink.pin;
    return NULL;
}

static void file_writer_destroy(struct strmbase_filter *iface)
{
    struct file_writer *filter = impl_from_strmbase_filter(iface);

    heap_free(filter->filename);
    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);
    heap_free(filter);
}

static HRESULT file_writer_init_stream(struct strmbase_filter *iface)
{
    struct file_writer *filter = impl_from_strmbase_filter(iface);
    HANDLE file;

    if ((file = CreateFileW(filter->filename, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, CREATE_ALWAYS, 0, NULL)) == INVALID_HANDLE_VALUE)
    {
        ERR("Failed to create %s, error %u.\n", debugstr_w(filter->filename), GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    filter->file = file;
    return S_OK;
}

static HRESULT file_writer_cleanup_stream(struct strmbase_filter *iface)
{
    struct file_writer *filter = impl_from_strmbase_filter(iface);

    CloseHandle(filter->file);
    return S_OK;
}

static struct strmbase_filter_ops filter_ops =
{
    .filter_query_interface = file_writer_query_interface,
    .filter_get_pin = file_writer_get_pin,
    .filter_destroy = file_writer_destroy,
    .filter_init_stream = file_writer_init_stream,
    .filter_cleanup_stream = file_writer_cleanup_stream,
};

static inline struct file_writer *impl_from_IFileSinkFilter(IFileSinkFilter *iface)
{
    return CONTAINING_RECORD(iface, struct file_writer, IFileSinkFilter_iface);
}

static HRESULT WINAPI filesinkfilter_QueryInterface(IFileSinkFilter *iface, REFIID iid, void **out)
{
    struct file_writer *filter = impl_from_IFileSinkFilter(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI filesinkfilter_AddRef(IFileSinkFilter *iface)
{
    struct file_writer *filter = impl_from_IFileSinkFilter(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI filesinkfilter_Release(IFileSinkFilter *iface)
{
    struct file_writer *filter = impl_from_IFileSinkFilter(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static HRESULT WINAPI filesinkfilter_SetFileName(IFileSinkFilter *iface,
        LPCOLESTR filename, const AM_MEDIA_TYPE *mt)
{
    struct file_writer *filter = impl_from_IFileSinkFilter(iface);
    WCHAR *new_filename;

    TRACE("filter %p, filename %s, mt %p, stub!\n", filter, debugstr_w(filename), mt);
    strmbase_dump_media_type(mt);

    if (mt)
        FIXME("Ignoring media type %p.\n", mt);

    if (!(new_filename = heap_alloc((strlenW(filename) + 1) * sizeof(WCHAR))))
        return E_OUTOFMEMORY;
    strcpyW(new_filename, filename);

    heap_free(filter->filename);
    filter->filename = new_filename;
    return S_OK;
}

static HRESULT WINAPI filesinkfilter_GetCurFile(IFileSinkFilter *iface,
        LPOLESTR *filename, AM_MEDIA_TYPE *mt)
{
    struct file_writer *filter = impl_from_IFileSinkFilter(iface);

    FIXME("filter %p, filename %p, mt %p, stub!\n", filter, filename, mt);

    return E_NOTIMPL;
}

static const IFileSinkFilterVtbl filesinkfilter_vtbl =
{
    filesinkfilter_QueryInterface,
    filesinkfilter_AddRef,
    filesinkfilter_Release,
    filesinkfilter_SetFileName,
    filesinkfilter_GetCurFile,
};

HRESULT file_writer_create(IUnknown *outer, IUnknown **out)
{
    static const WCHAR sink_name[] = {'i','n',0};
    struct file_writer *object;

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    strmbase_filter_init(&object->filter, outer, &CLSID_FileWriter, &filter_ops);
    object->IFileSinkFilter_iface.lpVtbl = &filesinkfilter_vtbl;

    strmbase_sink_init(&object->sink, &object->filter, sink_name, &sink_ops, NULL);

    TRACE("Created file writer %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}