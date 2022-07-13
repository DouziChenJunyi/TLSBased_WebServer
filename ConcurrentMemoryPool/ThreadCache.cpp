#include "ThreadCache.h"
#include "CentralCache.h"


//´ÓÖÐÐÄ»º´æ»ñÈ¡¶ÔÏó
// Ã¿Ò»´ÎÈ¡ÅúÁ¿µÄÊý¾Ý£¬ÒòÎªÃ¿´Îµ½CentralCacheÉêÇëÄÚ´æµÄÊ±ºòÊÇÐèÒª¼ÓËøµÄ
// ËùÒÔÒ»´Î¾Í¶àÉêÇëÒ»Ð©ÄÚ´æ¿é£¬·ÀÖ¹Ã¿´Îµ½CentralCacheÈ¥ÄÚ´æ¿éµÄÊ±ºò,¶à´Î¼ÓËøÔì³ÉÐ§ÂÊÎÊÌâ
void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{

    Freelist* freelist = &_freelist[index];
    // ²»ÊÇÃ¿´ÎÉêÇë10¸ö£¬¶øÊÇ½øÐÐÂýÔö³¤µÄ¹ý³Ì
    // µ¥¸ö¶ÔÏóÔ½Ð¡£¬ÉêÇëÄÚ´æ¿éµÄÊýÁ¿Ô½¶à
    // µ¥¸ö¶ÔÏóÔ½´ó£¬ÉêÇëÄÚ´æ¿éµÄÊýÁ¿Ô½Ð¡
    // ÉêÇë´ÎÊýÔ½¶à£¬ÊýÁ¿¶à
    // ´ÎÊýÉÙ,ÊýÁ¿ÉÙ
    size_t maxsize = freelist->MaxSize();
    size_t numtomove = std::min(SizeClass::NumMoveSize(size), maxsize);

    void* start = nullptr, *end = nullptr;
    // start£¬end·Ö±ð±íÊ¾È¡³öÀ´µÄÄÚ´æµÄ¿ªÊ¼µØÖ·ºÍ½áÊøµØÖ·
    // È¡³öÀ´µÄÄÚ´æÊÇÒ»¸öÁ´ÔÚÒ»ÆðµÄÄÚ´æ¶ÔÏó£¬ÐèÒªÊ×Î²±êÊ¶

    // batchsize±íÊ¾Êµ¼ÊÈ¡³öÀ´µÄÄÚ´æµÄ¸öÊý
    // batchsizeÓÐ¿ÉÄÜÐ¡ÓÚnum£¬±íÊ¾ÖÐÐÄ»º´æÃ»ÓÐÄÇÃ´¶à´óÐ¡µÄÄÚ´æ¿é
    size_t batchsize = CentralCache::Getinstence()->FetchRangeObj(start, end, numtomove, size);

    if (batchsize > 1)
    {
        freelist->PushRange(NEXT_OBJ(start), end, batchsize - 1);
    }

    if (batchsize >= freelist->MaxSize())
    {
        freelist->SetMaxSize(maxsize + 1);
    }

    return start;
}

//ÊÍ·Å¶ÔÏóÊ±£¬Á´±í¹ý³¤Ê±£¬»ØÊÕÄÚ´æ»Øµ½ÖÐÐÄ»º´æ
void ThreadCache::ListTooLong(Freelist* freelist, size_t size)
{
    void* start = freelist->PopRange();
    CentralCache::Getinstence()->ReleaseListToSpans(start, size);
}

//ÉêÇëºÍÊÍ·ÅÄÚ´æ¶ÔÏó
void* ThreadCache::Allocate(size_t size)
{
    size_t index = SizeClass::Index(size);//»ñÈ¡µ½Ïà¶ÔÓ¦µÄÎ»ÖÃ
    Freelist* freelist = &_freelist[index];
    if (!freelist->Empty())//ÔÚThreadCache´¦²»Îª¿ÕµÄ»°£¬Ö±½ÓÈ¡
    {
        return freelist->Pop();
    }
        // ×ÔÓÉÁ´±íÎª¿ÕµÄÒªÈ¥ÖÐÐÄ»º´æÖÐÄÃÈ¡ÄÚ´æ¶ÔÏó£¬Ò»´ÎÈ¡¶à¸ö·ÀÖ¹¶à´ÎÈ¥È¡¶ø¼ÓËø´øÀ´µÄ¿ªÏú
        // ¾ùºâ²ßÂÔ:Ã¿´ÎÖÐÐÄ¶Ñ·ÖÅä¸øThreadCache¶ÔÏóµÄ¸öÊýÊÇ¸öÂýÆô¶¯²ßÂÔ
        //          Ëæ×ÅÈ¡µÄ´ÎÊýÔö¼Ó¶øÄÚ´æ¶ÔÏó¸öÊýÔö¼Ó,·ÀÖ¹Ò»´Î¸øÆäËûÏß³Ì·ÖÅäÌ«¶à£¬¶øÁíÒ»Ð©Ïß³ÌÉêÇë
        //          ÄÚ´æ¶ÔÏóµÄÊ±ºò±ØÐëÈ¥PageCacheÈ¥È¡£¬´øÀ´Ð§ÂÊÎÊÌâ
    else
    {
        // ·ñÔòµÄ»°£¬´ÓÖÐÐÄ»º´æ´¦»ñÈ¡
        return FetchFromCentralCache(index, SizeClass::Roundup(size));
    }
}

void ThreadCache::Deallocate(void* ptr, size_t size)
{
    size_t index = SizeClass::Index(size);
    Freelist* freelist = &_freelist[index];
    freelist->Push(ptr);

    //Âú×ãÄ³¸öÌõ¼þÊ±(ÊÍ·Å»ØÒ»¸öÅúÁ¿µÄ¶ÔÏó)£¬ÊÍ·Å»ØÖÐÐÄ»º´æ
    if ( freelist->Size() >= freelist->MaxSize() )
    {
        ListTooLong(freelist, size);
    }
}


