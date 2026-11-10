#include "pool.hpp"

PoolAllocator<ITEM_CHUNK_SIZE>* getAllocator() {
	static PoolAllocator<ITEM_CHUNK_SIZE> pool;
	return &pool;
}

PoolAllocator<MSG_CHUNK_SIZE>* getMessageAllocator() {
	static PoolAllocator<MSG_CHUNK_SIZE> messagePool;
	return &messagePool;
}


#ifdef USE_MEM_POOL
void * operator new(std::size_t size) noexcept(false)
{
	return  PagesAllocator::instance()->allocate(size);
}


void operator delete(void * p) throw()
{
	PagesAllocator::instance()->free(p);
}

void operator delete(void * p, std::size_t size) throw()
{
	Q_UNUSED(size);
	PagesAllocator::instance()->free(p);
}
#endif
