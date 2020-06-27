import ctypes as c

from in3.libin3.lib_loader import libin3


class NativeRequest(c.Structure):
    """
    Based on in3/client/.h in3_request_t struct
    """
    _fields_ = [("payload", c.POINTER(c.c_char)),
                ("urls", c.POINTER(c.POINTER(c.c_char))),
                ("urls_len", c.c_int),
                ("results", c.c_void_p),
                ("timeout", c.c_uint32),
                ("times", c.c_uint32)]


class NativeResponse(c.Structure):
    """
    Based on in3/client/.h in3_response_t struct
    """


class In3Request:
    """
    C level abstraction of an Incubed request.
    """

    def __init__(self, in3_request: NativeRequest):
        self.in3_request = in3_request

    def url_at(self, index: int):
        """
        Gets the `index` url on the request url node list.
        Args:
            index (int): Positional argument to retrieve the url of a node from the list of urls. The total length should be retreived with `urls_len`
        Returns:
            fn_return (str): The url of a node to request a response from.
        """
        return c.string_at(libin3.in3_get_request_urls(self.in3_request)[index])

    def urls_len(self):
        """
        Gets the the size of the request url node list
        """
        return self.in3_request.contents.urls_len

    def payload(self):
        """
        Gets the payload to be sent
        """
        return c.string_at(self.in3_request.contents.payload)

    def timeout(self):
        """
        Get timeout of the request, `0` being no set timeout
        """
        return self.in3_request.contents.timeout


class In3Response:
    """
    C level abstraction of an Incubed response.
    """

    def __init__(self, in3_response: NativeResponse):
        self.in3_response = in3_response

    def success(self, index: int, msg: bytes):
        """
        Function to be invoked in order to write the result for the request in case of success
        Args:
            index (int): Positional argument related to which url on the `In3Request` list this response is associated with. Use `In3Request#url_at` to get the url. The value of both parameters are shared
            msg (str): The actual response to be returned to in3 client
        """
        libin3.in3_req_add_response(self.in3_response, index, False, msg, len(msg))

    def failure(self, index: int, msg: bytes):
        """
        Function to be invoked in order to write the result for the request in case of failure
        Args:
            index (int): Positional argument related to which url on the `In3Request` list this response is associated with. Use `In3Request#url_at` to get the url. The value of both parameters are shared.
            msg (str): The actual response to be returned to in3 client.
        """
        libin3.in3_req_add_response(self.in3_response, index, True, msg, len(msg))


def factory(transport_fn):
    """
    C level abstraction of a transport handler.
    Decorates a transport function augmenting its capabilities for native interoperability
    """

    def new(native_request: NativeRequest):
        request = In3Request(native_request)
        response = In3Response(native_request)
        return transport_fn(request, response)

    # the transport function to be implemented by the transport provider.
    # typedef in3_ret_t (*in3_transport_send)(in3_request_t* request);
    c_transport_fn = c.CFUNCTYPE(c.c_int, c.POINTER(NativeRequest))
    return c_transport_fn(new)
