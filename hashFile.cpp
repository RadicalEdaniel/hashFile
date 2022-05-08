//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// A programm that hashes a file chunk by chunk using sbdm hashing algorithm. 
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// 
//libs needed for boost
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/thread/thread.hpp>

//work with files
#include <fstream>

//work with console
#include <iostream>

//containers
#include <queue>

//work inside threads
#include <condition_variable>
#include <mutex>

//for sleeping in a thread
#include <chrono>
#include <thread>
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//a class for thread safe queue
template <class T> //using a template to not create 2 types of containers in here - uint and vector <char>. 
class concurrentQueue
{
public:
    // push a data to the queue
    void push(T &data)
    {      
        std::unique_lock<std::mutex> lock(m_mutex);//locking up the thread. Ours now!
        m_queue.push(std::move(data));//stealing data from the reference! Better than a plain copy.
        lock.unlock(); //Manual unlocking is done before notifying, to avoid waking up the waiting thread only to block again
        m_conditionVar.notify_one(); //If any threads are waiting  calling notify_one unblocks one of the waiting threads.
    }

    // pop data from the queue
    T pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_conditionVar.wait(lock, [&] { return !m_queue.empty(); });/*checks if queue is empty until it's not. 
                                                                Probably could improve it here by using wait_for instead to not wait forever
                                                                in some cases*/
        T data = m_queue.front(); //we take the data from the queue
        m_queue.pop();            //and pop it away!
        return data;
    }

private: 
    /*"we don't talk about Bruno", haha. I know it looks bad but i need to grad data from my uint vector at the end
    of the programm. So instead of unprivating queue i've decided to return a pointer to it*/
    std::queue<unsigned int>* threadUnsafeQueueReading()
    {        
        return &m_queue;
    }
private:
    //the queue itself! Full of mystery and thread safeness.
    std::queue<T> m_queue;
    //mutex to lock it up
    std::mutex m_mutex;
    //a condition variable to check if we have data in the queue
    std::condition_variable m_conditionVar;
    //we allow this function to see what is hidden
    friend void writeToFile(concurrentQueue<unsigned int>& outputQueue, std::ofstream& outFile);
};
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 
//a simple sdbm hash algorithm
unsigned int hash(std::vector<char> &data)
{
    unsigned int hash = 0;
    for (unsigned int i=0;i<data.size();i++)
        hash = data[i] + (hash << 7) + (hash << 13) - hash; //both lucky and unlucky
    return hash;
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
/*a function that is used by threads.Here we wait for data to come up, then we grab it, 
hash itand store it in the output queue*/
void doWork(concurrentQueue<std::vector<char>>& itemQueue, concurrentQueue<unsigned int>& resultQueue)
{
    std::vector<char> dataToHash;
    dataToHash =move(itemQueue.pop());
    unsigned int hashedData = hash(dataToHash);
    resultQueue.push(hashedData); 
    return;
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// 
//Takes data from outputQueue and stores it in a file. A friend of concurrentQueue.
void writeToFile(concurrentQueue<unsigned int> &outputQueue, std::ofstream &outFile)
{
    while (!outputQueue.threadUnsafeQueueReading()->empty())
    {
        outFile << outputQueue.threadUnsafeQueueReading()->front() << std::endl;
        outputQueue.threadUnsafeQueueReading()->pop();
    }
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

int main()
{
    //strings for cin
    std::string inFileName="", outFileName="", chunkSizeString="";
    //input and output data streams
    std::ifstream inFile;
    std::ofstream outFile;
    //a size of a chunk of memory we'll use
    size_t chunkSize=0;
    //2 custom classes for thread safe queues - input and output
    concurrentQueue<std::vector<char>> inputQueue;
    concurrentQueue<unsigned int> outputQueue;
    //creating a thread pool with all avaliable threads in this pc
    boost::asio::thread_pool pool(boost::thread::hardware_concurrency());
   
    try
    {    
        //getting our options
        std::cout << "Enter a path to the input file \n";
        std::getline(std::cin, inFileName);
        std::cout << "\nEnter a path to the output file \n";
        std::getline(std::cin, outFileName);
        std::cout << "\nEnter chunk size (empty = 1 MB) \n";
        std::getline(std::cin, chunkSizeString);
        if (chunkSizeString == ""|| std::stoi(chunkSizeString)==0)
            chunkSize = 1024 * 1024;
        else
            chunkSize = std::stoi(chunkSizeString);
    

        //checking if options are correct
        inFile.open(inFileName, std::ios::binary | std::ios::ate);
        if(!inFile.is_open())
            throw std::runtime_error("Wrong input file name! \n");
        outFile.open(outFileName);
        if (!outFile.is_open())
            throw std::runtime_error("Wrong output file name!\n");  
        /*checking if we have enough space for even ONE chunk of the file
        * ALSO !!! our buffer is a vector. I know that in the documentation 
        * is says to use smartpointers. But the vector is much better here in my opinion!
        * i could've used std::shared_ptr<char[]> ptr(new char[chunkSize]) but it has 
        * no way of tracking length and looks more cumbersome. 
        */
        std::vector<char> buffer(chunkSize , 0);
        if (buffer.size()!=chunkSize)
            throw std::runtime_error("Buffer couldn't allocate memory of the chunk!\n");

        //Sets the position of the next character to be extracted from the input stream at the begining of it
        inFile.seekg(0, inFile.beg);

        //our main loop
        while (true)
        {
            //reading the chunk amount from the file
            inFile.read(buffer.data(), buffer.size());
            std::streamsize count = inFile.gcount();          
            // If nothing has been read, break
            if (!count)
                break;
            //if not - we input new info into our queue
            inputQueue.push(buffer);

            //and hanging a "wanted!" poster on the nearest wall - creating a post for thread pool
            boost::asio::post(pool, [&inputQueue, &outputQueue]{doWork(inputQueue, outputQueue);});

            /*this right here is to be sure that we even have enough memory to move forward.
                if not - we wait until other threads are finished with their data and we have enough. 
            */
            /*At first i've used a CV/mutex wait here as well but then i've slept on it and realised 
            that... wasn't the best idea. It's not perfect - waiting in the main thread never is - 
            but i think this should do the job*/
            for(;;)
            {
                buffer.resize(chunkSize, 0); 
                if (buffer.size() == chunkSize)
                    break;
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));//to not freeze the programm
            }
            
        }
        //waiting for all threads to finish
        pool.join();
        //and printing our result
        writeToFile(outputQueue, outFile);
        std::cout << "All done!";
    }
    //just some general try catches.
    catch (std::bad_alloc& badAlloc)
    {
        std::cerr << "Not enough memory! " << badAlloc.what() << std::endl;
    }
    catch (std::exception &e)
    {
        std::cerr << "An unexpected error has occured! " << e.what() << std::endl;
    }
    
    std::cin.get();
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

