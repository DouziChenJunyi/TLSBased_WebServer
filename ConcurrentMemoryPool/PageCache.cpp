#include "PageCache.h"

PageCache PageCache::_inst;


//´ó¶ÔÏóÉêÇë£¬Ö±½Ó´ÓÏµÍ³
Span* PageCache::AllocBigPageObj(size_t size)
{
    assert(size > MAX_BYTES);

    size = SizeClass::_Roundup(size, PAGE_SHIFT); //¶ÔÆë
    size_t npage = size >> PAGE_SHIFT;
    if (npage < NPAGES)
    {
        Span* span = NewSpan(npage);
        span->_objsize = size;
        return span;
    }
    else
    {
        void* ptr = malloc( npage << PAGE_SHIFT);

        if (ptr == nullptr)
            throw std::bad_alloc();

        Span* span = new Span;
        span->_npage = npage;
        span->_pageid = (PageID)ptr >> PAGE_SHIFT;
        span->_objsize = npage << PAGE_SHIFT;

        _idspanmap[span->_pageid] = span;

        return span;
    }
}

void PageCache::FreeBigPageObj(void* ptr, Span* span)
{
    size_t npage = span->_objsize >> PAGE_SHIFT;
    if (npage < NPAGES) //Ïàµ±ÓÚ»¹ÊÇÐ¡ÓÚ128Ò³
    {
        span->_objsize = 0;
        ReleaseSpanToPageCache(span);
    }
    else
    {
        _idspanmap.erase(npage);
        delete span;
        free(ptr);
    }
}

Span* PageCache::NewSpan(size_t n)
{
    // ¼ÓËø£¬·ÀÖ¹¶à¸öÏß³ÌÍ¬Ê±µ½PageCacheÖÐÉêÇëspan
    // ÕâÀï±ØÐëÊÇ¸øÈ«¾Ö¼ÓËø£¬²»ÄÜµ¥¶ÀµÄ¸øÃ¿¸öÍ°¼ÓËø
    // Èç¹û¶ÔÓ¦Í°Ã»ÓÐspan,ÊÇÐèÒªÏòÏµÍ³ÉêÇëµÄ
    // ¿ÉÄÜ´æÔÚ¶à¸öÏß³ÌÍ¬Ê±ÏòÏµÍ³ÉêÇëÄÚ´æµÄ¿ÉÄÜ
    std::unique_lock<std::mutex> lock(_mutex);

    return _NewSpan(n);
}



Span* PageCache::_NewSpan(size_t n)
{
    assert(n < NPAGES);
    if (!_spanlist[n].Empty())
        return _spanlist[n].PopFront();

    for (size_t i = n + 1; i < NPAGES; ++i)
    {
        if (!_spanlist[i].Empty())
        {
            Span* span = _spanlist[i].PopFront();
            Span* splist = new Span;

            splist->_pageid = span->_pageid;
            splist->_npage = n;
            span->_pageid = span->_pageid + n;
            span->_npage = span->_npage - n;

            //splist->_pageid = span->_pageid + n;
            //span->_npage = splist->_npage - n;
            //span->_npage = n;

            for (size_t i = 0; i < n; ++i)
                _idspanmap[splist->_pageid + i] = splist;

            //_spanlist[splist->_npage].PushFront(splist);
            //return span;

            _spanlist[span->_npage].PushFront(span);
            return splist;
        }
    }

    Span* span = new Span;

    // µ½ÕâÀïËµÃ÷SpanListÖÐÃ»ÓÐºÏÊÊµÄspan,Ö»ÄÜÏòÏµÍ³ÉêÇë128Ò³µÄÄÚ´æ
// #ifdef _WIN32
// 	void* ptr = malloc(0, (NPAGES - 1)*(1 << PAGE_SHIFT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
// #else
// 	//  brk
// #endif

    void* ptr = malloc((NPAGES - 1)*(1 << PAGE_SHIFT));
    span->_pageid = (PageID)ptr >> PAGE_SHIFT;
    span->_npage = NPAGES - 1;

    for (size_t i = 0; i < span->_npage; ++i)
        _idspanmap[span->_pageid + i] = span;

    _spanlist[span->_npage].PushFront(span);  //·½À¨ºÅ
    return _NewSpan(n);
}

// »ñÈ¡´Ó¶ÔÏóµ½spanµÄÓ³Éä
Span* PageCache::MapObjectToSpan(void* obj)
{
    //¼ÆËãÒ³ºÅ
    PageID id = (PageID)obj >> PAGE_SHIFT;
    auto it = _idspanmap.find(id);
    if (it != _idspanmap.end())
    {
        return it->second;
    }
    else
    {
        assert(false);
        return nullptr;
    }
}



void PageCache::ReleaseSpanToPageCache(Span* cur)
{
    // ±ØÐëÉÏÈ«¾ÖËø,¿ÉÄÜ¶à¸öÏß³ÌÒ»Æð´ÓThreadCacheÖÐ¹é»¹Êý¾Ý
    std::unique_lock<std::mutex> lock(_mutex);


    // µ±ÊÍ·ÅµÄÄÚ´æÊÇ´óÓÚ128Ò³,Ö±½Ó½«ÄÚ´æ¹é»¹¸ø²Ù×÷ÏµÍ³,²»ÄÜºÏ²¢
    if (cur->_npage >= NPAGES)
    {
        void* ptr = (void*)(cur->_pageid << PAGE_SHIFT);
        // ¹é»¹Ö®Ç°É¾³ýµôÒ³µ½spanµÄÓ³Éä
        _idspanmap.erase(cur->_pageid);
        free(ptr);
        delete cur;
        return;
    }


    // ÏòÇ°ºÏ²¢
    while (1)
    {
        ////³¬¹ý128Ò³Ôò²»ºÏ²¢
        //if (cur->_npage > NPAGES - 1)
        //	break;

        PageID curid = cur->_pageid;
        PageID previd = curid - 1;
        auto it = _idspanmap.find(previd);

        // Ã»ÓÐÕÒµ½
        if (it == _idspanmap.end())
            break;

        // Ç°Ò»¸öspan²»¿ÕÏÐ
        if (it->second->_usecount != 0)
            break;

        Span* prev = it->second;

        //³¬¹ý128Ò³Ôò²»ºÏ²¢
        if (cur->_npage + prev->_npage > NPAGES - 1)
            break;


        // ÏÈ°Ñprev´ÓÁ´±íÖÐÒÆ³ý
        _spanlist[prev->_npage].Erase(prev);

        // ºÏ²¢
        prev->_npage += cur->_npage;
        //ÐÞÕýid->spanµÄÓ³Éä¹ØÏµ
        for (PageID i = 0; i < cur->_npage; ++i)
        {
            _idspanmap[cur->_pageid + i] = prev;
        }
        delete cur;

        // ¼ÌÐøÏòÇ°ºÏ²¢
        cur = prev;
    }


    //ÏòºóºÏ²¢
    while (1)
    {
        ////³¬¹ý128Ò³Ôò²»ºÏ²¢
        //if (cur->_npage > NPAGES - 1)
        //	break;

        PageID curid = cur->_pageid;
        PageID nextid = curid + cur->_npage;
        //std::map<PageID, Span*>::iterator it = _idspanmap.find(nextid);
        auto it = _idspanmap.find(nextid);

        if (it == _idspanmap.end())
            break;

        if (it->second->_usecount != 0)
            break;

        Span* next = it->second;

        //³¬¹ý128Ò³Ôò²»ºÏ²¢
        if (cur->_npage + next->_npage >= NPAGES - 1)
            break;

        _spanlist[next->_npage].Erase(next);

        cur->_npage += next->_npage;
        //ÐÞÕýid->SpanµÄÓ³Éä¹ØÏµ
        for (PageID i = 0; i < next->_npage; ++i)
        {
            _idspanmap[next->_pageid + i] = cur;
        }

        delete next;
    }

    // ×îºó½«ºÏ²¢ºÃµÄspan²åÈëµ½spanÁ´ÖÐ
    _spanlist[cur->_npage].PushFront(cur);
}
