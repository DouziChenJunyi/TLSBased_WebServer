#include "CentralCache.h"
#include "PageCache.h"

CentralCache CentralCache::_inst;

Span* CentralCache::GetOneSpan(SpanList& spanlist, size_t byte_size)
{
    Span* span = spanlist.Begin();
    while (span != spanlist.End())//µ±Ç°ÕÒµ½Ò»¸öspan
    {
        if (span->_list != nullptr)
            return span;
        else
            span = span->_next;
    }

    // ×ßµ½Õâ¶ù£¬ËµÃ÷Ç°ÃæÃ»ÓÐ»ñÈ¡µ½span,¶¼ÊÇ¿ÕµÄ£¬µ½ÏÂÒ»²ãpagecache»ñÈ¡span
    Span* newspan = PageCache::GetInstence()->NewSpan(SizeClass::NumMovePage(byte_size));
    // ½«spanÒ³ÇÐ·Ö³ÉÐèÒªµÄ¶ÔÏó²¢Á´½ÓÆðÀ´
    char* cur = (char*)(newspan->_pageid << PAGE_SHIFT);
    char* end = cur + (newspan->_npage << PAGE_SHIFT);
    newspan->_list = cur;
    newspan->_objsize = byte_size;

    while (cur + byte_size < end)
    {
        char* next = cur + byte_size;
        NEXT_OBJ(cur) = next;
        cur = next;
    }
    NEXT_OBJ(cur) = nullptr;

    spanlist.PushFront(newspan);

    return newspan;
}


//»ñÈ¡Ò»¸öÅúÁ¿µÄÄÚ´æ¶ÔÏó
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t n, size_t byte_size)
{
    size_t index = SizeClass::Index(byte_size);
    SpanList& spanlist = _spanlist[index];//¸³Öµ->¿½±´¹¹Ôì

    std::unique_lock<std::mutex> lock(spanlist._mutex);

    Span* span = GetOneSpan(spanlist, byte_size);
    //µ½Õâ¶ùÒÑ¾­»ñÈ¡µ½Ò»¸önewspan

    //´ÓspanÖÐ»ñÈ¡range¶ÔÏó
    size_t batchsize = 0;
    void* prev = nullptr;//ÌáÇ°±£´æÇ°Ò»¸ö
    void* cur = span->_list;//ÓÃcurÀ´±éÀú£¬Íùºó×ß
    for (size_t i = 0; i < n; ++i)
    {
        prev = cur;
        cur = NEXT_OBJ(cur);
        ++batchsize;
        if (cur == nullptr)//ËæÊ±ÅÐ¶ÏcurÊÇ·ñÎª¿Õ£¬Îª¿ÕµÄ»°£¬ÌáÇ°Í£Ö¹
            break;
    }

    start = span->_list;
    end = prev;

    span->_list = cur;
    span->_usecount += batchsize;

    //½«¿ÕµÄspanÒÆµ½×îºó£¬±£³Ö·Ç¿ÕµÄspanÔÚÇ°Ãæ
    if (span->_list == nullptr)
    {
        spanlist.Erase(span);
        spanlist.PushBack(span);
    }

    //spanlist.Unlock();

    return batchsize;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
    size_t index = SizeClass::Index(size);
    SpanList& spanlist = _spanlist[index];

    //½«Ëø·ÅÔÚÑ­»·ÍâÃæ
    // CentralCache:¶Ôµ±Ç°Í°½øÐÐ¼ÓËø(Í°Ëø)£¬¼õÐ¡ËøµÄÁ£¶È
    // PageCache:±ØÐë¶ÔÕû¸öSpanListÈ«¾Ö¼ÓËø
    // ÒòÎª¿ÉÄÜ´æÔÚ¶à¸öÏß³ÌÍ¬Ê±È¥ÏµÍ³ÉêÇëÄÚ´æµÄÇé¿ö
    //spanlist.Lock();
    std::unique_lock<std::mutex> lock(spanlist._mutex);

    while (start)
    {
        void* next = NEXT_OBJ(start);

        ////µ½Ê±ºò¼ÇµÃ¼ÓËø
        //spanlist.Lock(); // ¹¹³ÉÁËºÜ¶àµÄËø¾ºÕù

        Span* span = PageCache::GetInstence()->MapObjectToSpan(start);
        NEXT_OBJ(start) = span->_list;
        span->_list = start;
        //µ±Ò»¸öspanµÄ¶ÔÏóÈ«²¿ÊÍ·Å»ØÀ´µÄÊ±ºò£¬½«span»¹¸øpagecache,²¢ÇÒ×öÒ³ºÏ²¢
        if (--span->_usecount == 0)
        {
            spanlist.Erase(span);
            PageCache::GetInstence()->ReleaseSpanToPageCache(span);
        }

        //spanlist.Unlock();

        start = next;
    }

    //spanlist.Unlock();
}