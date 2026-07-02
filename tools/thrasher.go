package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"os"
	"strings"
	"time"
)

func remoteDelete(remote string) error {
	req, err := http.NewRequest(http.MethodDelete, remote, nil)
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		return fmt.Errorf("remote_delete: wrong status code %d", resp.StatusCode)
	}
	return nil
}

func remotePut(remote string, length int64, body io.Reader) error {
	req, err := http.NewRequest(http.MethodPut, remote, body)
	if err != nil {
		return err
	}
	req.ContentLength = length
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated && resp.StatusCode != http.StatusNoContent {
		return fmt.Errorf("remote_put: wrong status code %d", resp.StatusCode)
	}
	return nil
}

func remoteGet(remote string) (string, error) {
	resp, err := http.Get(remote)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", errors.New(fmt.Sprintf("remote_get: wrong status code %d", resp.StatusCode))
	}
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", err
	}
	return string(body), nil
}

func main() {
	base := flag.String("base", "http://localhost:3000", "minikv master base URL")
	count := flag.Int("n", 10000, "write/read/delete cycles")
	concurrency := flag.Int("c", 16, "concurrent goroutines")
	flag.Parse()

	rand.Seed(time.Now().UTC().UnixNano())

	reqs := make(chan string, *count*2)
	resp := make(chan bool, *count*2)
	fmt.Println("starting thrasher")

	http.DefaultTransport.(*http.Transport).MaxIdleConnsPerHost = 100

	for i := 0; i < *concurrency; i++ {
		go func() {
			for {
				key := <-reqs
				value := fmt.Sprintf("value-%d", rand.Int())
				if err := remotePut(*base+"/"+key, int64(len(value)), strings.NewReader(value)); err != nil {
					fmt.Println("PUT FAILED", err)
					resp <- false
					continue
				}

				ss, err := remoteGet(*base + "/" + key)
				if err != nil || ss != value {
					fmt.Println("GET FAILED", err, ss, value)
					resp <- false
					continue
				}

				if err := remoteDelete(*base + "/" + key); err != nil {
					fmt.Println("DELETE FAILED", err)
					resp <- false
					continue
				}
				resp <- true
			}
		}()
	}

	start := time.Now()
	for i := 0; i < *count; i++ {
		key := fmt.Sprintf("benchmark-%d", rand.Int())
		reqs <- key
	}

	for i := 0; i < *count; i++ {
		if <-resp == false {
			fmt.Println("ERROR on", i)
			os.Exit(1)
		}
	}

	elapsed := time.Since(start)
	fmt.Println(*count, "write/read/delete in", elapsed)
	fmt.Printf("thats %.2f/sec\n", float64(*count)/elapsed.Seconds())
}
