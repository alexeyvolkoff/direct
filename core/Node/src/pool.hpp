#ifndef POOL_HPP
#define POOL_HPP
#include <QVector>
#include <memory>

#define ITEM_CHUNK_SIZE 512
#define MSG_CHUNK_SIZE 128



class PagesAllocator {
public:
	struct Page
	{
		char memory[4096];
	};

	void* allocatePage() {
		void *ptr = ::calloc(1, sizeof(Page));
		if (ptr) {
			m_pagesAllocated ++;
		}
		return ptr;
	}

	void releasePage(void *ptr) {
		if (ptr) {
			m_pagesAllocated --;
		}
	}

	int pagesAllocated() {return m_pagesAllocated; }
	int allocated() {return m_allocated; }

	void* allocate(size_t size) {
		void *ptr = ::calloc(1, size);
		if (ptr) {
			m_allocated ++;
		}
		return ptr;
	}

	void free(void *ptr) {
		m_allocated --;
		::free(ptr);
	}


	static PagesAllocator* instance() {
		static PagesAllocator pagesAllocator;
		return  &pagesAllocator;
	}

private:
	int m_pagesAllocated = 0;
	int m_allocated = 0;


	PagesAllocator() :m_pagesAllocated(0)  {

	}

};


template<size_t ChunkSize>
class PoolAllocator
{

	struct Node
	{
		 Node *previous;
		 PagesAllocator::Page *page;
		 int allocated;
		 char chunk[ChunkSize - 2 * sizeof(void *) - sizeof (int)];
	};

public:
	PoolAllocator()
	{

	}

	~PoolAllocator()
	{

	}

	int allocated() { return m_allocated; }

	void *allocate(size_t size)
	{
		Q_ASSERT(size <= ChunkSize);
		Node *node = arena()->pop();
		if (node) ++m_allocated;
		return node ? node->chunk : nullptr;
	}

	void free(void *ptr)
	{
		unsigned char* bytePtr = reinterpret_cast<unsigned char*> (ptr);
		Node *node = reinterpret_cast<Node*>(bytePtr - 2 * sizeof(void *) - sizeof (int));
		arena()->push(node);
		--m_allocated;
	}

private:
	int m_allocated = 0;

	struct Arena
		{
			QVector<PagesAllocator::Page*> pages;
			Node *stack = nullptr;

			void push(void *ptr)
			{
				Node *top = static_cast<Node*>(ptr);
				top->previous = stack;
				top->allocated = 0;
				stack = top;
				releasePage(top->page);
			}

			Node  *pop()
			{
				if (!stack && !grow())
						return nullptr;
				Node *top = stack;
				stack = stack->previous;
				top->allocated = 1;
				return top;
			}

			void initPage(PagesAllocator::Page *page) {
				Node *newStack = reinterpret_cast<Node*>(page->memory);
				newStack->previous = stack;
				newStack->allocated = 0;
				newStack->page = page;

				const size_t numChunks = sizeof(PagesAllocator::Page) / sizeof(Node);
				for (size_t i = 0; i < numChunks - 1; ++i, ++newStack) {
					newStack[1].previous = newStack;
					newStack[1].allocated = 0;
					newStack[1].page = page;
				}
				stack = newStack;
			}

			bool releasePage(PagesAllocator::Page *page) {
				Node *node = reinterpret_cast<Node*>(page->memory);
				if (node) {
					const size_t numChunks = sizeof(PagesAllocator::Page) / sizeof(Node);
					for (size_t i = 0; i < numChunks - 1; ++i, ++node) {
						if (node->allocated) return false;
					}
				}
				//::free(page);
				//stack = node->previous;
				initPage(page);
				return true;
			}

			bool grow()
			{
				  void *ptr = PagesAllocator::instance()->allocatePage();
				  if (!ptr) {

					  return false;
				  }

				  PagesAllocator::Page *newPage = static_cast<PagesAllocator::Page*>(ptr);
				  initPage(newPage);
				  pages.append(newPage);
				  return true;
			}

			~Arena()
			{
				for (auto page : pages) {
					//releasePage(page);
					PagesAllocator::instance()->releasePage(page);
				}
			}
		};

		inline thread_local static std::unique_ptr<Arena> current_thread;

		Arena *arena()
		{
			Arena *a = current_thread.get();
			if (!a) {
				a = new Arena();
				current_thread.reset(a);
			}
			return a;
		}
};

PoolAllocator<ITEM_CHUNK_SIZE>* getAllocator();

PoolAllocator<MSG_CHUNK_SIZE>* getMessageAllocator();



#endif // POOL_HPP
