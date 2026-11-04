"""Physical labels for the JBOX-S3 connectors; protocol node IDs stay stable."""
from __future__ import annotations

GPIO_LIST = (1, 2, 3, 4, 5, 6, 7, 8, 9, 18, 21, 38, 39, 40, 41, 42, 47, 48)
DIRECT_HEADER = ((None, 48, 47, 42, 41), (18, 21, 38, 39, 40))
NET_COLORS = ('#58c4ff', '#f5b85b', '#b59bff', '#52d6b0',
              '#ff8ba4', '#d5dd75', '#78a2ff', '#dd94e6')


def connector_node(connector: str, pin: int) -> int:
    """Adjacent odd/even pins are physically shorted, e.g. CN1.39 == CN1.40."""
    if connector not in ('CN1', 'CN2') or type(pin) is not int or not 1 <= pin <= 40:
        raise ValueError('端子必须是 CN1/CN2 的 1～40 脚')
    return (0 if connector == 'CN1' else 20) + (pin - 1) // 2


def connector_rows(connector: str) -> tuple[tuple[tuple[int, int], ...], ...]:
    return tuple(tuple((pin, connector_node(connector, pin)) for pin in range(start, 0, -2))
                 for start in (39, 40))


def node_label(node: int) -> str:
    if 0 <= node < 40:
        connector = 'CN1' if node < 20 else 'CN2'
        odd = (node % 20) * 2 + 1
        return f'{connector} · {odd}/{odd + 1}'
    if 40 <= node <= 48:
        return f'GPIO{node - 39} (EXT{node - 39})'
    if node in (49, 50, 51):
        return {49: '5V', 50: '3.3V', 51: 'GND'}[node]
    raise ValueError('节点编号必须为 0～51')


def validate_nets(nets) -> list[list[int]]:
    if not isinstance(nets, list) or len(nets) > 8:
        raise ValueError('最多可配置 8 组网络')
    result, seen = [], set()
    for net in nets:
        if not isinstance(net, list):
            raise ValueError('网络必须是节点列表')
        if any(type(node) is not int or not 0 <= node < 52 for node in net):
            raise ValueError('节点编号必须为 0～51 的整数')
        if len(set(net)) != len(net) or seen.intersection(net):
            raise ValueError('同一个物理节点只能属于一组网络（奇偶脚为同一节点）')
        power = set(net).intersection((49, 50, 51))
        mcu = set(net).intersection(range(40, 49))
        if len(power) > 1:
            raise ValueError('5V、3.3V、GND 不能接在同一网络')
        if power and mcu:
            raise ValueError('固件不允许电源节点与 MCU 的 EXT 直接相连')
        if len(mcu) > 1:
            raise ValueError('固件不允许两个 MCU EXT 直接相连')
        seen.update(net)
        result.append(sorted(net))
    return result + [[] for _ in range(8 - len(result))]


class RouteDesign:
    def __init__(self):
        self.nets = [[] for _ in range(8)]
        self.active = 0

    def load(self, nets):
        self.nets = validate_nets(nets)

    def toggle(self, node: int):
        candidate = [list(net) for net in self.nets]
        if node in candidate[self.active]:
            candidate[self.active].remove(node)
        else:
            for net in candidate:
                if node in net:
                    net.remove(node)
            candidate[self.active].append(node)
        self.load(candidate)

    def group_of(self, node: int):
        return next((i for i, net in enumerate(self.nets) if node in net), None)

    def export(self):
        return {'format': 'jbox-routes-v1', 'nets': self.nets,
                'labels': [[node_label(n) for n in net] for net in self.nets]}

    def import_document(self, document):
        if not isinstance(document, dict) or document.get('format') != 'jbox-routes-v1':
            raise ValueError('不是 JBOX 连线方案文件')
        self.load(document.get('nets'))
