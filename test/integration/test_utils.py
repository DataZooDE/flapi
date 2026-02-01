"""
Test utility functions for FLAPI integration tests
"""
import requests
import time
import psutil
import os
from typing import Dict, Any, Optional, List
import json


def create_test_product(base_url: str, product_name: str = "Test Product", 
                        supplier_id: int = 1, category_id: int = 1,
                        **kwargs) -> Optional[Dict[str, Any]]:
    """
    Create a test product via POST endpoint.
    
    Args:
        base_url: Base URL of the FLAPI server
        product_name: Name of the product
        supplier_id: Supplier ID
        category_id: Category ID
        **kwargs: Additional product fields
        
    Returns:
        Created product data or None if creation failed
    """
    url = f"{base_url}/northwind/products/"
    payload = {
        "product_name": product_name,
        "supplier_id": supplier_id,
        "category_id": category_id,
        **kwargs
    }
    
    response = requests.post(url, json=payload, timeout=10)
    
    if response.status_code in [200, 201]:
        data = response.json()
        return data.get("data", [None])[0] if isinstance(data.get("data"), list) else data
    return None


def delete_test_product(base_url: str, product_id: int) -> bool:
    """
    Delete a test product via DELETE endpoint.
    
    Args:
        base_url: Base URL of the FLAPI server
        product_id: Product ID to delete
        
    Returns:
        True if deletion was successful, False otherwise
    """
    url = f"{base_url}/northwind/products/{product_id}"
    response = requests.delete(url, timeout=10)
    return response.status_code in [200, 204]


def cleanup_test_products(base_url: str, product_ids: List[int]):
    """
    Clean up multiple test products.
    
    Args:
        base_url: Base URL of the FLAPI server
        product_ids: List of product IDs to delete
    """
    for product_id in product_ids:
        try:
            delete_test_product(base_url, product_id)
        except Exception as e:
            print(f"Warning: Failed to delete product {product_id}: {e}")


def measure_performance(func, *args, **kwargs) -> Dict[str, float]:
    """
    Measure the performance of a function.
    
    Args:
        func: Function to measure
        *args: Function arguments
        **kwargs: Function keyword arguments
        
    Returns:
        Dictionary with 'execution_time' and other metrics
    """
    start_time = time.time()
    start_memory = psutil.Process().memory_info().rss / 1024 / 1024  # MB
    
    result = func(*args, **kwargs)
    
    end_time = time.time()
    end_memory = psutil.Process().memory_info().rss / 1024 / 1024  # MB
    
    return {
        "execution_time": end_time - start_time,
        "memory_delta": end_memory - start_memory,
        "result": result
    }


def monitor_memory(interval: float = 1.0, duration: float = 10.0) -> List[float]:
    """
    Monitor memory usage over time.
    
    Args:
        interval: Time between measurements (seconds)
        duration: Total duration to monitor (seconds)
        
    Returns:
        List of memory usage values (MB)
    """
    process = psutil.Process()
    measurements = []
    start_time = time.time()
    
    while time.time() - start_time < duration:
        memory_mb = process.memory_info().rss / 1024 / 1024
        measurements.append(memory_mb)
        time.sleep(interval)
    
    return measurements


def generate_test_payload(size: int = 1000) -> Dict[str, Any]:
    """
    Generate a test JSON payload of specified size.
    
    Args:
        size: Approximate size in bytes
        
    Returns:
        Dictionary payload
    """
    # Generate a string that's approximately 'size' bytes
    string_length = max(1, (size - 100) // 2)  # Approximate
    test_string = "x" * string_length
    
    return {
        "product_name": test_string[:50],  # Truncate to valid length
        "supplier_id": 1,
        "category_id": 1,
        "quantity_per_unit": test_string[:50],
        "description": test_string  # Can be longer
    }


def wait_for_server(base_url: str, max_retries: int = 30, 
                   retry_interval: float = 1.0) -> bool:
    """
    Wait for the FLAPI server to be ready.
    
    Args:
        base_url: Base URL of the FLAPI server
        max_retries: Maximum number of retry attempts
        retry_interval: Time to wait between retries (seconds)
        
    Returns:
        True if server is ready, False otherwise
    """
    for _ in range(max_retries):
        try:
            response = requests.get(base_url, timeout=5)
            if response.status_code == 200:
                return True
        except requests.exceptions.RequestException:
            pass
        
        time.sleep(retry_interval)
    
    return False


def get_process_memory() -> float:
    """
    Get current process memory usage in MB.
    
    Returns:
        Memory usage in MB
    """
    return psutil.Process().memory_info().rss / 1024 / 1024


def check_memory_leak(initial_memory: float, current_memory: float, 
                     threshold_mb: float = 100.0) -> bool:
    """
    Check if memory usage has increased beyond threshold.
    
    Args:
        initial_memory: Initial memory usage (MB)
        current_memory: Current memory usage (MB)
        threshold_mb: Threshold for memory leak detection (MB)
        
    Returns:
        True if memory leak detected, False otherwise
    """
    return (current_memory - initial_memory) > threshold_mb


def make_concurrent_requests(base_url: str, endpoint: str, 
                            method: str = "GET", num_requests: int = 100,
                            payload: Optional[Dict] = None, 
                            headers: Optional[Dict] = None) -> List[Dict[str, Any]]:
    """
    Make concurrent HTTP requests and return results.
    
    Args:
        base_url: Base URL of the FLAPI server
        endpoint: Endpoint path
        method: HTTP method
        num_requests: Number of concurrent requests
        payload: Request payload (for POST/PUT)
        headers: Request headers
        
    Returns:
        List of response dictionaries with status_code and response_time
    """
    import concurrent.futures
    
    url = f"{base_url}{endpoint}"
    results = []
    
    def make_request():
        start = time.time()
        try:
            if method == "GET":
                response = requests.get(url, headers=headers, timeout=30)
            elif method == "POST":
                response = requests.post(url, json=payload, headers=headers, timeout=30)
            elif method == "PUT":
                response = requests.put(url, json=payload, headers=headers, timeout=30)
            elif method == "DELETE":
                response = requests.delete(url, headers=headers, timeout=30)
            else:
                return {"status_code": 405, "response_time": 0}
            
            response_time = time.time() - start
            return {
                "status_code": response.status_code,
                "response_time": response_time
            }
        except Exception as e:
            return {
                "status_code": 0,
                "response_time": time.time() - start,
                "error": str(e)
            }
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=num_requests) as executor:
        futures = [executor.submit(make_request) for _ in range(num_requests)]
        results = [future.result() for future in concurrent.futures.as_completed(futures)]
    
    return results


def calculate_percentiles(values: List[float], percentiles: List[int] = [50, 90, 95, 99]) -> Dict[int, float]:
    """
    Calculate percentiles from a list of values.
    
    Args:
        values: List of numeric values
        percentiles: List of percentile values to calculate
        
    Returns:
        Dictionary mapping percentile to value
    """
    if not values:
        return {}
    
    sorted_values = sorted(values)
    result = {}
    
    for p in percentiles:
        index = int(len(sorted_values) * p / 100)
        index = min(index, len(sorted_values) - 1)
        result[p] = sorted_values[index]
    
    return result


def test_utility_module_smoke():
    """Sanity check so pytest collects this helper module without error."""
    assert True
