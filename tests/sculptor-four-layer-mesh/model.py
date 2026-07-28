import torch
from torch import nn


class FourLinearLayerApplication(nn.Module):
    def __init__(self):
        super().__init__()
        self.first = nn.Linear(4, 4)
        self.second = nn.Linear(4, 4)
        self.third = nn.Linear(4, 4)
        self.fourth = nn.Linear(4, 4)

        with torch.no_grad():
            self.first.weight.copy_(torch.eye(4))
            self.first.bias.copy_(torch.tensor([1.0, 1.0, 1.0, 1.0]))

            self.second.weight.copy_(2.0 * torch.eye(4))
            self.second.bias.copy_(torch.tensor([0.0, 1.0, 2.0, 3.0]))

            self.third.weight.copy_(
                torch.tensor(
                    [
                        [0.0, 0.0, 0.0, 1.0],
                        [0.0, 0.0, 1.0, 0.0],
                        [0.0, 1.0, 0.0, 0.0],
                        [1.0, 0.0, 0.0, 0.0],
                    ]
                )
            )
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

    def forward(self, value):
        value = self.first(value)
        value = self.second(value)
        value = self.third(value)
        return self.fourth(value)


SAMPLE_INPUT = torch.tensor([[1.0, 2.0, 3.0, 4.0]], dtype=torch.float32)
EXPECTED = torch.tensor([[21.0, 10.0, 20.0, 15.0]], dtype=torch.float32)


if __name__ == "__main__":
    result = FourLinearLayerApplication()(SAMPLE_INPUT)
    print(f"PyTorch result: {result.tolist()}")
    if not torch.equal(result, EXPECTED):
        raise SystemExit("unexpected PyTorch result")
