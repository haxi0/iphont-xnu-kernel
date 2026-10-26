"""
Every failure iphont reports has a stable code (what an agent branches on), a message (what happened)
and, where one is known, a hint (what to do next). The exit status follows from the code.
"""

EXIT = {
    'usage': 2,
    'absent': 3, 'not_in_loader': 3, 'no_fastboot': 3, 'no_kernel': 3,
    'phone_busy': 4, 'link_busy': 4,
    'timeout': 5, 'no_answer': 5, 'agent_timeout': 5,
    'refused': 6, 'unsupported': 6, 'old_loader': 6, 'old_kernel': 6,
    'device_error': 1, 'check_failed': 1, 'run_failed': 1, 'panic': 1, 'build_failed': 1, 'not_found': 1,
}


class Fail(Exception):
    def __init__(self, code, message, hint=None, detail=None):
        super().__init__(message)
        self.code, self.message, self.hint, self.detail = code, message, hint, detail

    def exit_status(self):
        return EXIT.get(self.code, 7)

    def as_json(self):
        e = {'code': self.code, 'message': self.message}
        if self.hint:
            e['hint'] = self.hint
        if self.detail:
            e['detail'] = self.detail
        return e
