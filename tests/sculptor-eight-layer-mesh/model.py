import torch
from torch import nn


class EightLinearLayerApplication(nn.Module):
    def __init__(self):
        super().__init__()
        self.first = nn.Linear(4, 4)
        self.second = nn.Linear(4, 4)
        self.third = nn.Linear(4, 4)
        self.fourth = nn.Linear(4, 4)
        self.fifth = nn.Linear(4, 4)
        self.sixth = nn.Linear(4, 4)
        self.seventh = nn.Linear(4, 4)
        self.eighth = nn.Linear(4, 4)

        identity = torch.eye(4)
        reverse = torch.tensor(
            [
                [0.0, 0.0, 0.0, 1.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [1.0, 0.0, 0.0, 0.0],
            ]
        )
        with torch.no_grad():
            self.first.weight.copy_(identity)
            self.first.bias.copy_(torch.tensor([1.0, 1.0, 1.0, 1.0]))

            self.second.weight.copy_(2.0 * identity)
            self.second.bias.copy_(torch.tensor([0.0, 1.0, 2.0, 3.0]))

            self.third.weight.copy_(reverse)
            self.third.bias.copy_(torch.tensor([-1.0, -1.0, -1.0, -1.0]))

            self.fourth.weight.copy_(
                torch.tensor(
                    [
                        [1.0, 1.0, 0.0, 0.0],
                        [0.0, 0.0, 1.0, 1.0],
                        [1.0, 0.0, 1.0, 0.0],
                        [0.0, 1.0, 0.0, 1.0],
                    ]
                )
            )
            self.fourth.bias.copy_(torch.tensor([0.0, 1.0, 2.0, 3.0]))

            self.fifth.weight.copy_(identity)
            self.fifth.bias.copy_(torch.tensor([1.0, 2.0, 3.0, 4.0]))

            self.sixth.weight.copy_(
                torch.tensor(
                    [
                        [1.0, 0.0, 0.0, 0.0],
                        [0.0, 2.0, 0.0, 0.0],
                        [0.0, 0.0, 1.0, 0.0],
                        [0.0, 0.0, 0.0, 2.0],
                    ]
                )
            )
            self.sixth.bias.zero_()

            self.seventh.weight.copy_(
                torch.tensor(
                    [
                        [0.0, 1.0, 0.0, 0.0],
                        [0.0, 0.0, 1.0, 0.0],
                        [0.0, 0.0, 0.0, 1.0],
                        [1.0, 0.0, 0.0, 0.0],
                    ]
                )
            )
            self.seventh.bias.copy_(torch.tensor([-4.0, -3.0, -2.0, -1.0]))

            self.eighth.weight.copy_(
                torch.tensor(
                    [
                        [1.0, 0.0, 1.0, 0.0],
                        [0.0, 1.0, 0.0, 1.0],
                        [1.0, 1.0, 0.0, 0.0],
                        [0.0, 0.0, 1.0, 1.0],
                    ]
                )
            )
            self.eighth.bias.copy_(torch.tensor([0.0, 1.0, 2.0, 3.0]))

    def forward(self, value):
        value = self.first(value)
        value = self.second(value)
        value = self.third(value)
        value = self.fourth(value)
        value = self.fifth(value)
        value = self.sixth(value)
        value = self.seventh(value)
        return self.eighth(value)


SAMPLE_INPUT = torch.tensor([[1.0, 2.0, 3.0, 4.0]], dtype=torch.float32)
EXPECTED = torch.tensor([[56.0, 42.0, 42.0, 60.0]], dtype=torch.float32)


if __name__ == "__main__":
    result = EightLinearLayerApplication()(SAMPLE_INPUT)
    print(f"PyTorch result: {result.tolist()}")
    if not torch.equal(result, EXPECTED):
        raise SystemExit("unexpected PyTorch result")
